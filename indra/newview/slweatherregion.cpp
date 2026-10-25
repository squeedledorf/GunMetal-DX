/**
 * @file slweatherregion.cpp
 * @brief Weather (#783 M2): the region source. See slweatherregion.h and
 *        doc/weather_region_protocol.md.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slweatherregion.h"

#include "llagent.h"
#include "llenvironment.h"
#include "llframetimer.h"
#include "llparcel.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>

namespace
{
constexpr F64 kPollSecs = 1.0;
// #895: through a border crossing the previous region's feed is held this long (s) at most while
// the new region's agent parcel is unknown (no dip in a snowfall that continues on the other side).
constexpr F64 kCrossHoldSecs = 10.0;
// Tag defaults (the local weather's setting defaults).
constexpr F32 kDefWindDir = 225.f, kDefWindSpeed = 3.f, kDefVar = 0.3f, kDefGust = 0.5f, kDefTrans = 30.f;
// #954: the precipitation's bands (pvar=) when a tag does not say (the WeatherPrecipVariation default).
constexpr F32 kDefFallVar = 0.35f;
// Sky-derived rain: per window of synced time, a chance by the cloud coverage.
constexpr F64 kSkyWindow = 600.0;          // s
constexpr F32 kSkyTrans = 90.f;            // s: sky rain comes and goes slowly
constexpr F32 kSkyRainFrom = 0.5f, kSkyRainFull = 0.95f, kSkyRainMaxChance = 0.85f;

U64 mix64(U64 x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
F32 u01(U64 h) { return (F32)(h >> 40) * (1.f / 16777216.f); }
F32 smooth01(F32 e0, F32 e1, F32 x)
{
    const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// A UUID's 32-bit seed: FNV-1a over its 16 bytes (stable across platforms).
U32 uuidSeed(const LLUUID& id)
{
    U32 h = 2166136261u;
    for (U32 i = 0; i < UUID_BYTES; ++i) h = (h ^ id.mData[i]) * 16777619u;
    return h;
}

bool parseF32(const std::string& v, F32& out)
{
    const char* b = v.data();
    const char* e = b + v.size();
    F32 x = 0.f;
    const auto r = std::from_chars(b, e, x);   // (locale-independent: '.' always)
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(x)) return false;
    out = x;
    return true;
}

// A unix time (decimal seconds, whole: since=). Integer parse -- an F32 cannot hold 1.8e9 to the second.
bool parseUnix(const std::string& v, F64& out)
{
    const char* b = v.data();
    const char* e = b + v.size();
    S64 x = 0;
    const auto r = std::from_chars(b, e, x, 10);
    if (r.ec != std::errc() || r.ptr != e || x < 0 || x > SLWeatherRegion::kMaxSince) return false;
    out = (F64)x;
    return true;
}

bool parseU32(const std::string& v, U32& out)
{
    const char* b = v.data();
    const char* e = b + v.size();
    int base = 10;
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) { b += 2; base = 16; }
    U32 x = 0;
    const auto r = std::from_chars(b, e, x, base);
    if (r.ec != std::errc() || r.ptr != e) return false;
    out = x;
    return true;
}

struct Ctl
{
    F64          nextPoll = 0.0;
    SLWeatherRegion::EStatus status = SLWeatherRegion::STATUS_OFF;
    bool         fed = false;
    // The parsed tag of the last description seen (re-parsed only on a change).
    LLUUID       regionId;
    S32          parcelId = -1;
    std::string  desc;
    bool         haveTag = false, tagErrors = false;
    SLWeather::Target tag;
    U32          resetSeen = 0;   // #895: SLWeather::resetSerial() last seen
    U32          placeSeen = 0;   // #895: SLWeather::placeSerial() last seen (a teleport)
    F64          holdUntil = 0.0; // #895: a crossing's feed held until then (0: not holding)
};
Ctl& ctl()
{
    static Ctl c;
    return c;
}

void feed(Ctl& c, const SLWeather::Target& t)
{
    SLWeather::instance().setRegionTarget(t);
    c.fed = true;
}

void unfeed(Ctl& c)
{
    if (c.fed) SLWeather::instance().clearRegionTarget();
    c.fed = false;
}

// The shared (parcel / region) sky, not the user's own: the current sky while a shared
// environment is what is shown, else the shared instance's (may lag while a personal
// environment is selected: it is only stepped while shown).
LLSettingsSky::ptr_t sharedSky()
{
    LLEnvironment& env = LLEnvironment::instance();
    const LLEnvironment::EnvSelection_t sel = env.getSelectedEnvironment();
    if (sel == LLEnvironment::ENV_PARCEL || sel == LLEnvironment::ENV_REGION || sel == LLEnvironment::ENV_DEFAULT)
        return env.getCurrentSky();
    LLEnvironment::DayInstance::ptr_t inst = env.getSharedEnvironmentInstance();
    return inst ? inst->getSky() : LLSettingsSky::ptr_t();
}

// Sky rain (slweatherregion.h, SKY). false: dry in this window (or no sky).
bool skyTarget(U32 seed, SLWeather::Target& t)
{
    LLSettingsSky::ptr_t sky = sharedSky();
    if (!sky) return false;
    const F32 cover = llclamp(sky->getCloudShadow(), 0.f, 1.f);
    const F32 chance = kSkyRainMaxChance * smooth01(kSkyRainFrom, kSkyRainFull, cover);
    if (chance <= 0.f) return false;
    const S64 window = (S64)std::floor(SLWeather::syncedSeconds() / kSkyWindow);
    const U64 h = mix64(((U64)seed << 32) ^ mix64((U64)window));
    if (u01(h) >= chance) return false;
    t = SLWeather::Target();
    t.active = true;
    t.setFall(SLWeather::PRECIP_RAIN, SLWeather::fallFromAmount(SLWeather::PRECIP_RAIN,
              llclamp(0.15f + 0.7f * smooth01(0.6f, 1.f, cover) * (0.6f + 0.4f * u01(mix64(h))), 0.f, 1.f)));
    t.fallVariation = kDefFallVar;   // (#954: bands -- the same seed, so the region agrees)
    t.windDirDeg = kDefWindDir;
    t.windSpeed = 2.f + 5.f * smooth01(0.7f, 1.f, cover);
    t.windVariation = kDefVar;
    t.windGustiness = kDefGust;
    t.windDirVariation = 0.f;
    // The heaviest skies sometimes thunder (every other such window).
    t.storm = (u01(mix64(h + 1)) < 0.5f) ? 0.4f * smooth01(0.85f, 1.f, cover) : 0.f;
    t.fog = SLWeather::precipFog(t.rain, 0.f);
    t.cloud = cover;   // (what the sky already shows: the renderer adds nothing)
    t.transitionSecs = kSkyTrans;
    t.seed = seed;   // (the same across windows: the gusts stay continuous)
    return true;
}

// The first "[slwx" tag (any case, followed by a space or ']') up to its closing ']'.
// `at` is the '[', `end` the ']' (inclusive span). false: none (an unterminated one is none).
bool findTagSpan(const std::string& text, size_t& at, size_t& end)
{
    std::string low(text);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    at = 0;
    for (;;)
    {
        at = low.find("[slwx", at);
        if (at == std::string::npos) return false;
        const size_t after = at + 5;
        if (after < low.size() && (low[after] == ']' || std::isspace((unsigned char)low[after]))) break;
        at = after;
    }
    end = low.find(']', at);
    return end != std::string::npos;
}

// A number, fixed to `decimals`, trailing zeros and a leading "0" dropped (".6", "225", "8.5").
std::string fmtNum(F32 v, int decimals)
{
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
    std::string s(buf, r.ptr);
    if (s.find('.') != std::string::npos)
    {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s.size() > 1 && s[0] == '0' && s[1] == '.') s.erase(0, 1);
    if (s.empty() || s == "-0") s = "0";
    return s;
}

F32 quant(F32 v, F32 step) { return std::round(v / step) * step; }
} // namespace

// static
std::string SLWeatherRegion::formatTag(const SLWeather::Target& in)
{
    if (!in.active) return "[slwx off]";
    // Quantize first: what is written is exactly what parseTag will read back.
    const F32 rain = llclamp(quant(in.rain, 0.01f), 0.f, 1.f), snow = llclamp(quant(in.snow, 0.01f), 0.f, 1.f);
    const F32 storm = llclamp(quant(in.storm, 0.01f), 0.f, 1.f);
    const F32 var = llclamp(quant(in.windVariation, 0.01f), 0.f, 1.f), gust = llclamp(quant(in.windGustiness, 0.01f), 0.f, 1.f);
    const F32 dirvar = llclamp(std::round(in.windDirVariation), 0.f, 180.f);
    const F32 dir = std::fmod(std::fmod(std::round(in.windDirDeg), 360.f) + 360.f, 360.f);
    const F32 speed = llclamp(quant(in.windSpeed, 0.1f), 0.f, 40.f);
    const F32 trans = llclamp(std::round(in.transitionSecs), 0.f, 600.f);
    const F32 fog = llclamp(quant(in.fog, 0.01f), 0.f, 1.f), cloud = llclamp(quant(in.cloud, 0.01f), 0.f, 1.f);

    std::string tag = "[slwx";
    // #954: the 0..1 amount (every viewer reads it) AND the physical rate (newer viewers prefer it:
    // the amount's 0.01 steps are ~3 mm/h apart near the top of the ladder). The rates' steps keep
    // the amount they give back within ~0.003 of the original even at the ladder's foot (round trip).
    const F32 rainMm = llclamp(quant(in.rainFall, 0.01f), 0.f, SLWeather::kRainFallMax);
    const F32 snowCm = llclamp(quant(in.snowFall, 0.001f), 0.f, SLWeather::kSnowFallMax);
    if (rain > 0.f) tag += " rain=" + fmtNum(rain, 2);
    if (rain > 0.f && rainMm > 0.f) tag += " rainmm=" + fmtNum(rainMm, 2);
    if (snow > 0.f) tag += " snow=" + fmtNum(snow, 2);
    if (snow > 0.f && snowCm > 0.f) tag += " snowcm=" + fmtNum(snowCm, 3);
    const F32 pvar = llclamp(quant(in.fallVariation, 0.01f), 0.f, 1.f);
    if ((rain > 0.f || snow > 0.f) && std::fabs(pvar - kDefFallVar) > 0.005f) tag += " pvar=" + fmtNum(pvar, 2);
    if (storm > 0.f) tag += " storm=" + fmtNum(storm, 2);
    if (std::fabs(dir - kDefWindDir) > 0.5f || std::fabs(speed - kDefWindSpeed) > 0.05f)
        tag += " wind=" + fmtNum(dir, 0) + "@" + fmtNum(speed, 1);
    if (std::fabs(var - kDefVar) > 0.005f) tag += " var=" + fmtNum(var, 2);
    if (std::fabs(gust - kDefGust) > 0.005f) tag += " gust=" + fmtNum(gust, 2);
    if (dirvar > 0.5f) tag += " wdirvar=" + fmtNum(dirvar, 0);
    // fog / cloud: only when they differ from what the parser derives from the rest.
    if (std::fabs(fog - quant(SLWeather::precipFog(rain, snow), 0.01f)) > 0.005f) tag += " fog=" + fmtNum(fog, 2);
    if (std::fabs(cloud - quant(std::max({rain, snow, storm > 0.f ? 0.5f + 0.5f * storm : 0.f}), 0.01f)) > 0.005f)
        tag += " cloud=" + fmtNum(cloud, 2);
    if (std::fabs(trans - kDefTrans) > 0.5f) tag += " trans=" + fmtNum(trans, 0);
    // #783 SHARED SNOW DEPTH (slweather.h): depth (cm), rate (mm/h), since (whole unix seconds).
    if (in.haveDepth) tag += " depth=" + fmtNum(llclamp(quant(in.snowDepthAt, 0.01f), 0.f, SLWeather::kSnowDepthMax), 2);
    if (in.haveRate) tag += " rate=" + fmtNum(llclamp(quant(in.snowRate, 0.001f), -kMaxSnowRate, kMaxSnowRate), 3);
    if (in.haveSince)
    {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof(buf), (S64)std::clamp(std::floor(in.snowSince), 0.0, (F64)kMaxSince));
        tag += " since=" + std::string(buf, r.ptr);
    }
    // (No seed: the parcel's default seed is the one every viewer there shares.)
    if (tag.find('=') == std::string::npos) tag += " rain=0";   // (a tag with nothing in it: explicit "dry")
    return tag + "]";
}

// static
bool SLWeatherRegion::editDescription(const std::string& desc, const std::string& tag, std::string& out)
{
    size_t at = 0, end = 0;
    const bool have = findTagSpan(desc, at, end);
    out = desc;
    if (tag.empty())
    {
        if (!have) return false;
        out.erase(at, end - at + 1);
        // Drop one separating blank (the one before the tag, else the one after).
        if (at > 0 && std::isspace((unsigned char)out[at - 1])) out.erase(at - 1, 1);
        else if (at < out.size() && std::isspace((unsigned char)out[at])) out.erase(at, 1);
        return true;
    }
    if (have)
    {
        out.replace(at, end - at + 1, tag);
    }
    else
    {
        if (!out.empty() && !std::isspace((unsigned char)out.back())) out += ' ';
        out += tag;
    }
    return true;
}

// static
bool SLWeatherRegion::tagRoundTrips(const std::string& tag, const SLWeather::Target& want)
{
    SLWeather::Target got;
    std::string bad;
    if (!parseTag(tag, 0, got, &bad) || !bad.empty()) return false;
    auto eq = [](F32 a, F32 b, F32 tol) { return std::fabs(a - b) <= tol; };
    if (!want.active) return !(got.rain > 0.f || got.snow > 0.f || got.storm > 0.f);
    return eq(got.rain, want.rain, 0.006f) && eq(got.snow, want.snow, 0.006f) && eq(got.storm, want.storm, 0.006f)
        && eq(got.windSpeed, want.windSpeed, 0.06f) && eq(got.windVariation, want.windVariation, 0.006f)
        && eq(got.windGustiness, want.windGustiness, 0.006f) && eq(got.windDirVariation, want.windDirVariation, 0.51f) && eq(got.fog, want.fog, 0.006f)
        && eq(got.cloud, want.cloud, 0.006f) && eq(got.transitionSecs, want.transitionSecs, 0.51f)
        && (want.windSpeed < 0.05f || eq(std::fabs(std::remainder(got.windDirDeg - want.windDirDeg, 360.f)), 0.f, 0.51f))
        && got.haveDepth == want.haveDepth && got.haveRate == want.haveRate && got.haveSince == want.haveSince
        && (!want.haveDepth || eq(got.snowDepthAt, want.snowDepthAt, 0.006f))
        && (!want.haveRate || eq(got.snowRate, want.snowRate, 0.0006f))
        && (!want.haveSince || std::fabs(got.snowSince - std::floor(want.snowSince)) < 0.5)
        // #954 (the rates written only with their amount; pvar only with precipitation)
        && (want.rain < 0.005f || eq(got.rainFall, want.rainFall, 0.0051f))
        && (want.snow < 0.005f || eq(got.snowFall, want.snowFall, 0.00051f))
        && (want.rain < 0.005f && want.snow < 0.005f ? true : eq(got.fallVariation, want.fallVariation, 0.006f));
}

// static
bool SLWeatherRegion::parseTag(const std::string& text, U32 default_seed, SLWeather::Target& out, std::string* errors)
{
    // Find "[slwx" (any case) followed by a space or ']'.
    size_t at = 0, end = 0;
    if (!findTagSpan(text, at, end)) return false;   // (none, or unterminated: not a tag)
    std::string low(text);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });

    SLWeather::Target t;
    t.active = true;
    t.windDirDeg = kDefWindDir;
    t.windSpeed = kDefWindSpeed;
    t.windVariation = kDefVar;
    t.windGustiness = kDefGust;
    t.windDirVariation = 0.f;
    t.transitionSecs = kDefTrans;
    t.seed = default_seed;
    t.fallVariation = kDefFallVar;
    bool haveFog = false, haveCloud = false, off = false;
    F32 rainMm = -1.f, snowCm = -1.f;   // (#954: rainmm= / snowcm=, -1: not given)
    std::string bad;
    const std::string body = low.substr(at + 5, end - at - 5);
    size_t i = 0;
    while (i < body.size())
    {
        while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
        size_t j = i;
        while (j < body.size() && !std::isspace((unsigned char)body[j])) ++j;
        if (j == i) break;
        const std::string tok = body.substr(i, j - i);
        i = j;
        if (tok == "off" || tok == "clear")
        {
            // Explicitly no weather here (it eases out), wherever it stands in the tag.
            off = true;
            continue;
        }
        const size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            bad += (bad.empty() ? "" : " ") + tok;
            continue;
        }
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        F32 f = 0.f;
        bool ok = true;
        if (k == "rain" || k == "snow" || k == "var" || k == "gust" || k == "storm" || k == "fog" || k == "cloud")
        {
            ok = parseF32(v, f);
            f = llclamp(f, 0.f, 1.f);
            if (ok)
            {
                if (k == "rain") t.rain = f;
                else if (k == "snow") t.snow = f;
                else if (k == "var") t.windVariation = f;
                else if (k == "gust") t.windGustiness = f;
                else if (k == "storm") t.storm = f;
                else if (k == "fog") { t.fog = f; haveFog = true; }
                else { t.cloud = f; haveCloud = true; }
            }
        }
        else if (k == "wind")
        {
            // DIR@SPEED (degrees the wind comes FROM, m/s) or SPEED alone.
            const size_t a = v.find('@');
            F32 dir = t.windDirDeg, speed = 0.f;
            ok = a == std::string::npos ? parseF32(v, speed) : (parseF32(v.substr(0, a), dir) && parseF32(v.substr(a + 1), speed));
            if (ok)
            {
                t.windDirDeg = std::fmod(std::fmod(dir, 360.f) + 360.f, 360.f);
                t.windSpeed = llclamp(speed, 0.f, 40.f);
            }
        }
        else if (k == "wdirvar")
        {
            ok = parseF32(v, f);
            if (ok) t.windDirVariation = llclamp(f, 0.f, 180.f);
        }
        else if (k == "trans")
        {
            ok = parseF32(v, f);
            if (ok) t.transitionSecs = llclamp(f, 0.f, 600.f);
        }
        else if (k == "seed")
        {
            U32 s = 0;
            ok = parseU32(v, s);
            if (ok) t.seed = s;
        }
        // #783 SHARED SNOW DEPTH (slweather.h): depth= m (0..kSnowDepthMax: the viewer caps it at
        // its own WeatherSnowMaxDepth), rate= m/h (+-kMaxSnowRate), since= unix seconds.
        else if (k == "depth")
        {
            ok = parseF32(v, f);
            if (ok) { t.snowDepthAt = llclamp(f, 0.f, SLWeather::kSnowDepthMax); t.haveDepth = true; }
        }
        else if (k == "rate")
        {
            ok = parseF32(v, f);
            if (ok) { t.snowRate = llclamp(f, -kMaxSnowRate, kMaxSnowRate); t.haveRate = true; }
        }
        else if (k == "since")
        {
            F64 u = 0.0;
            ok = parseUnix(v, u);
            if (ok) { t.snowSince = u; t.haveSince = true; }
        }
        // #954 PRECIPITATION RATE (slweather.h): rainmm= mm/h, snowcm= cm/h of snowfall (they win over
        // rain= / snow=, which older viewers read), pvar= the bands 0..1.
        else if (k == "rainmm" || k == "snowcm")
        {
            ok = parseF32(v, f);
            if (ok)
            {
                if (k == "rainmm") rainMm = llclamp(f, 0.f, SLWeather::kRainFallMax);
                else snowCm = llclamp(f, 0.f, SLWeather::kSnowFallMax);
            }
        }
        else if (k == "pvar")
        {
            ok = parseF32(v, f);
            if (ok) t.fallVariation = llclamp(f, 0.f, 1.f);
        }
        // (Unknown keys are ignored silently: later versions may add some.)
        if (!ok) bad += (bad.empty() ? "" : " ") + tok;
    }
    // #954: the rates (and the amounts derived from them, when given), else the rates of the amounts.
    t.setFall(SLWeather::PRECIP_RAIN, rainMm >= 0.f ? rainMm : SLWeather::fallFromAmount(SLWeather::PRECIP_RAIN, t.rain));
    t.setFall(SLWeather::PRECIP_SNOW, snowCm >= 0.f ? snowCm : SLWeather::fallFromAmount(SLWeather::PRECIP_SNOW, t.snow));
    t.haveSnowFall = snowCm >= 0.f;
    if (off)
    {
        t.rain = t.snow = t.storm = 0.f;
        t.rainFall = t.snowFall = 0.f;
        t.haveSnowFall = false;
        t.windSpeed = 0.f;
        t.haveDepth = t.haveRate = t.haveSince = false;   // (no weather here: no shared snow either)
    }
    if (!haveFog) t.fog = SLWeather::precipFog(t.rain, t.snow);
    if (!haveCloud) t.cloud = std::max({t.rain, t.snow, t.storm > 0.f ? 0.5f + 0.5f * t.storm : 0.f});
    out = t;
    if (errors) *errors = bad;
    return true;
}

// static
SLWeatherRegion::EStatus SLWeatherRegion::status()
{
    return ctl().status;
}

// static
void SLWeatherRegion::update()
{
    static LLCachedControl<bool> s_allow(gSavedSettings, "WeatherAllowRegion", false);
    static LLCachedControl<bool> s_sky(gSavedSettings, "WeatherFromEnvironment", false);
    Ctl& c = ctl();
    SLWeather::watchRegion();   // (#895: the teleport / crossing reset; idempotent)
    // #895: the weather was reset (master off) or the agent teleported to a new place: it dropped
    // our target, so the next poll is now and nothing cached from the old place is assumed fed.
    if (SLWeather::instance().resetSerial() != c.resetSeen || SLWeather::instance().placeSerial() != c.placeSeen)
    {
        c.resetSeen = SLWeather::instance().resetSerial();
        c.placeSeen = SLWeather::instance().placeSerial();
        c.holdUntil = 0.0;
        c.fed = false;
        c.nextPoll = 0.0;
        c.regionId.setNull();
        c.parcelId = -1;
        c.desc.clear();
        c.haveTag = c.tagErrors = false;
    }
    if (!SLWeather::masterEnabled() || (!s_allow && !s_sky))   // (master off: the region source does nothing at all)
    {
        unfeed(c);
        c.status = STATUS_OFF;
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < c.nextPoll) return;
    c.nextPoll = now + kPollSecs;

    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        unfeed(c);
        c.status = STATUS_NONE;
        return;
    }
    const U32 regionSeed = uuidSeed(region->getRegionID());

    // The parcel's tag first.
    if (s_allow)
    {
        LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
        LLParcel* parcel = pm->getAgentParcel();
        // #895: only the parcel the agent is ON. Through a teleport (and after a crossing, until
        // the new region's ParcelProperties arrive) the agent parcel is still the previous
        // region's: its tag must not follow the agent to the new region. Unknown = nothing yet.
        // A teleport inside the region keeps what is fed until the destination parcel is known
        // (no dip in the weather for a hop across the parcel); so does a border CROSSING, for up
        // to kCrossHoldSecs (slweather.h PLACES: a snowfall that continues on the other side
        // does not ease out and back in). A teleport to another region fed nothing any more
        // (its new place dropped the target): nothing to hold.
        if (parcel && parcel->getRegionID() != region->getRegionID())
        {
            if (c.fed && !pm->getTeleportInProgress() && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
            {
                if (c.holdUntil <= 0.0) c.holdUntil = now + kCrossHoldSecs;
                if (now < c.holdUntil)
                {
                    c.nextPoll = 0.0;   // (re-checked every frame until the agent's parcel is known)
                    return;
                }
                LL_INFOS("Weather") << "parcel weather: the new region's agent parcel is still unknown after "
                                    << kCrossHoldSecs << " s -- the previous region's weather is no longer held" << LL_ENDL;
            }
            // (logged once per agent region: "waiting" vs a parcel whose region was never set)
            static LLUUID s_logged;
            if (s_logged != region->getRegionID())
            {
                s_logged = region->getRegionID();
                LL_INFOS("Weather") << "parcel weather: waiting for this region's agent parcel (the known one is "
                                    << (parcel->getRegionID().isNull() ? "of no region" : "the previous region's") << ")"
                                    << LL_ENDL;
            }
            unfeed(c);
            c.status = STATUS_NONE;
            c.nextPoll = 0.0;   // (re-checked every frame until the agent's parcel is known)
            return;
        }
        if (parcel && pm->getTeleportInProgress())
        {
            c.nextPoll = 0.0;
            return;
        }
        c.holdUntil = 0.0;
        if (parcel)
        {
            if (region->getRegionID() != c.regionId || parcel->getLocalID() != c.parcelId || parcel->getDesc() != c.desc)
            {
                c.regionId = region->getRegionID();
                c.parcelId = parcel->getLocalID();
                c.desc = parcel->getDesc();
                const U32 seed = regionSeed ^ (U32)(mix64((U64)(U32)c.parcelId) >> 32);
                std::string bad;
                c.haveTag = parseTag(c.desc, seed, c.tag, &bad);
                c.tagErrors = c.haveTag && !bad.empty();
                if (c.haveTag)
                {
                    std::string depth = "none (local build-up)";
                    if (c.tag.haveDepth)
                        depth = llformat("%.2f m, rate %.3f m/h, since %s", c.tag.snowDepthAt, c.tag.haveRate ? c.tag.snowRate : 0.f,
                                         c.tag.haveSince ? llformat("%lld", (long long)c.tag.snowSince).c_str() : "none");
                    else if (c.tag.haveRate)
                        depth = llformat("rate %.3f m/h only (the level stays local)", c.tag.snowRate);
                    LL_INFOS("Weather") << "parcel weather tag: rain " << c.tag.rain << " (" << c.tag.rainFall << " mm/h) snow "
                                        << c.tag.snow << " (" << c.tag.snowFall << " cm/h) bands " << c.tag.fallVariation << " wind "
                                        << c.tag.windDirDeg << "@" << c.tag.windSpeed << " storm " << c.tag.storm << " seed "
                                        << c.tag.seed << " snow depth " << depth << (bad.empty() ? "" : "; not understood: ") << bad
                                        << LL_ENDL;
                }
            }
            if (c.haveTag)
            {
                feed(c, c.tag);
                c.status = c.tagErrors ? STATUS_PARCEL_ERRORS : STATUS_PARCEL;
                SLWeather::instance().noteSourceSettled(region->getHandle(), c.parcelId);   // (#895)
                return;
            }
        }
    }
    // #895: this place is evaluated (fed below or not): a crossing / parcel change / teleport's
    // destination is known now (slweather.h PLACES).
    {
        LLParcel* ap = s_allow ? LLViewerParcelMgr::getInstance()->getAgentParcel() : nullptr;
        if (!s_allow || ap) SLWeather::instance().noteSourceSettled(region->getHandle(), ap ? ap->getLocalID() : -1);   // (no parcel yet: unknown)
    }
    // Then the sky.
    if (s_sky)
    {
        SLWeather::Target t;
        if (skyTarget(regionSeed, t))
        {
            feed(c, t);
            c.status = STATUS_SKY_RAIN;
        }
        else
        {
            unfeed(c);
            c.status = STATUS_SKY_DRY;
        }
        return;
    }
    unfeed(c);
    c.status = STATUS_NONE;
}
