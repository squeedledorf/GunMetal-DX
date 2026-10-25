/**
 * @file slweatheraudio.cpp
 * @brief Weather ambience (#783): see slweatheraudio.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slweatheraudio.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llaudiofiledecode.h"
#include "llcleanacoustics.h"
#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "slweather.h"
#include "workqueue.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
// ---- the beds (app_settings/sounds/weather/<name>.ogg) ----
enum ELayer : U32
{
    RAIN_LIGHT = 0, RAIN_MEDIUM, RAIN_HEAVY,
    ROOF_WOOD, ROOF_WOOD_HEAVY, ROOF_METAL, ROOF_METAL_HEAVY, ROOF_GLASS, ROOF_TENT,
    DRIP, HAIL_OUTDOOR, HAIL_ROOF,
    WIND_LIGHT, WIND_GUST, WIND_HOWL, WIND_INDOOR,
    SNOW_AMBIENCE, SNOW_INTERIOR, SNOW_WIND_MUFFLED,
    // #894 wind through the openings around the listener (PLACED at the opening: mono)
    WIND_WHISTLE, WIND_WHISTLE_B, WIND_HOWL_WINDOW, RUSH_LIGHT, RUSH_GUST,
    L_COUNT
};
struct LayerDef
{
    const char* name;
    F32         gain;           // manifest.json default_gain (pack 1.0.0); loudness is baked per file
    bool        mono = false;   // #894: decoded down to mono (a stereo bed placed at an opening)
    bool        placed = false; // #894: placed at an opening's direction (setLocalLoopOffset)
    bool        sparse = false; // #1012: a sparse bed (recognisable single drops): gets the random drop layer
};
constexpr LayerDef kLayers[L_COUNT] = {
    {"rain_light_loop", 1.f, false, false, true},      {"rain_medium_loop", 1.f},       {"rain_heavy_loop", 1.f},
    {"roof_wood_loop", 1.f},       {"roof_wood_heavy_loop", 1.f},   {"roof_metal_loop", 1.f},
    {"roof_metal_heavy_loop", 1.f},{"roof_glass_loop", 1.f},        {"roof_tent_loop", 1.f},
    {"drip_loop", 1.f, false, false, true},            {"hail_outdoor_loop", 1.f},      {"hail_roof_loop", 1.f},
    {"wind_light_loop", 1.f},      {"wind_gust_loop", 1.f},         {"wind_howl_loop", 1.f},
    {"wind_indoor_loop", 1.f},
    {"snow_ambience_loop", 1.f},   {"snow_interior_loop", 1.f},     {"snow_wind_muffled_loop", 1.f},
    {"wind_whistle_loop", 0.6f, false, true},   {"wind_whistle_b_loop", 0.6f, false, true},
    {"wind_howl_window_loop", 1.f},
    {"wind_light_loop", 1.f, true, true},       {"wind_gust_loop", 1.f, true, true},
};
// Cross-fade groups: a bed still loading lends its share to the nearest loaded one.
constexpr U32 kGroupRain[] = {RAIN_LIGHT, RAIN_MEDIUM, RAIN_HEAVY};
constexpr U32 kGroupRoof[] = {ROOF_WOOD, ROOF_WOOD_HEAVY, ROOF_METAL, ROOF_METAL_HEAVY, ROOF_GLASS, ROOF_TENT};
constexpr U32 kGroupWind[] = {WIND_LIGHT, WIND_GUST, WIND_HOWL};
constexpr U32 kGroupRush[] = {RUSH_LIGHT, RUSH_GUST};

// ---- the levels ----
constexpr F32 kRainPeak[3] = {0.25f, 0.6f, 1.f};     // rain amount at which each rain bed is alone
constexpr F32 kWindPeak[3] = {4.f, 9.f, 15.f};       // m/s, likewise for the wind beds
constexpr F32 kRoofHeavy0 = 0.45f, kRoofHeavy1 = 0.85f;   // rain: roof bed -> heavy roof bed
constexpr F32 kMinRoofShare = 0.1f;   // roof material shares below this are ignored
constexpr F32 kSnowWind = 0.35f;         // the outdoor wind's level in snow (snow is quiet)
// #954 PRECIPITATION RATE: a downpour (rain amount 0.75 .. 1: ~23 .. 100 mm/h) lifts the rain beds
// by up to this; a blizzard (snow amount 0.7 .. 0.9: ~4 .. 9 cm/h, x wind 8 .. 16 m/s) undoes the
// snow's quieting of the outdoor wind and lifts it by up to this.
constexpr F32 kDownpourLift = 0.3f;
constexpr F32 kBlizzardLift = 0.35f;
constexpr F32 kDripGain = 0.7f;           // drips sit under the roof drumming
constexpr F32 kRunoffDripGain = 0.55f;    // M4: an eave / stream pouring nearby, heard outdoors too
constexpr F32 kHailRoofSoft = 0.6f;       // hail on wood / canvas: softer than on metal / glass
constexpr F32 kLevelTau = 1.f;            // s: rain / snow / wetness easing
constexpr F32 kSpeedTau = 0.3f;           // s: wind speed easing (the gusts must come through)
constexpr F32 kMuffleTau = 0.4f;          // s: cover easing (also hides the readback's latency)
constexpr F32 kAttack = 0.6f;             // s: a layer's gain rising
constexpr F32 kRelease = 1.8f;            // s: falling (stepping out lingers, like an eave)
// #895 CUT (SLWeather::resetSerial: master off, teleport): the beds fall this fast for kCutSecs
// (~95 % down in 0.5 s, gain 0 = paused) and the thunder fades out over kCutSecs, then stops.
constexpr F32 kCutRelease = 0.15f;        // s
constexpr F64 kCutSecs = 0.6;
constexpr F32 kUnderwaterGain = 0.4f;
constexpr F32 kSilent = 1e-3f;
constexpr F32 kLoadAt = 3e-3f;            // a layer wanting more than this gets its bed loaded
constexpr F32 kFreeAfter = 10.f;          // s of silence before a bed is freed
constexpr F64 kRetrySecs = 30.0;          // a failed decode / loop creation retries this much later
constexpr U32 kMaxTries = 3;              // ... this many times, then the bed is dropped
constexpr U32 kMaxInFlight = 2;           // decodes queued at once
constexpr U64 kPcmBudget = 128ull << 20;  // decoded beds alive (AL buffers), bytes
constexpr U64 kInFlightGuess = 16ull << 20;   // a decode's size before it is known (the largest bed)
// #894 OPENINGS (slweatheraudio.h, WIND THROUGH OPENINGS): the solid angle (sr) bounds of the size
// classes -- crack / small / window-sized / wide -- (smooth, a partition of one), the windward
// weighting (an opening's outward horizontal direction against the direction the wind comes FROM:
// lee openings still let a little in; a skylight takes the middle), the windward solid angle at
// which each class is at full level, the direction / gust easing, where a placed bed sits.
constexpr F32 kOpenCrack0 = 0.03f, kOpenCrack1 = 0.10f;
constexpr F32 kOpenSmall0 = 0.15f, kOpenSmall1 = 0.35f;
constexpr F32 kOpenWide0 = 0.5f, kOpenWide1 = 1.0f;
constexpr F32 kOpenLee = 0.2f, kOpenUp = 0.5f;
constexpr F32 kOpenFullCrack = 0.05f, kOpenFullSmall = 0.12f, kOpenFullWindow = 0.5f, kOpenFullWide = 1.2f;
constexpr F32 kOpenDirTau = 0.5f;          // s
constexpr F32 kOpenPlaceMin = 1.5f, kOpenPlaceMax = 8.f;   // m
constexpr F32 kOpenFallbackSr = 1.2f;      // no rays: the roof map's openness x this, all "wide", unplaced
constexpr F32 kOpenIndoorDuck = 0.5f;      // the sealed-room bed gives way this much to a full opening
constexpr F32 kWhistlePitchGust = 0.15f;   // whistle pitch per unit of gust (relative to the mean wind)

// #1012 LOOP VARIATION (slweatheraudio.h): two playheads per bed cross-fading to a fresh start point,
// a sparse layer of drops cut from the sparse beds, a slow level drift.
constexpr F32 kEnvFineSecs = 0.02f;        // the drop finder's window
constexpr U32 kEnvCoarse = 12;             // fine windows per coarse envelope window (0.24 s: start points)
constexpr U32 kMaxDrops = 12;              // slices cut per sparse bed
constexpr F32 kDropMinSep = 0.6f;          // s between the transients chosen
constexpr F32 kDropMin = 0.08f, kDropMax = 0.25f;   // s: slice length
constexpr F32 kDropLead = 0.015f;          // s of the slice before its transient
constexpr F32 kDropRate = 0.7f;            // drops per second at a bed's full level (scales with it)
constexpr F32 kDropGain = 0.3f;            // x the bed's own level: detail, not a feature
constexpr F32 kDropDistMin = 1.5f, kDropDistMax = 6.f;   // m around the listener
constexpr F32 kSwapMin = 10.f, kSwapMax = 25.f;   // s between playhead swaps (per bed, random)
constexpr F32 kXfadeMin = 2.f, kXfadeMax = 4.f;   // s: the equal-power cross-fade
constexpr F32 kMinSepSecs = 4.f;           // a new start point is this far from the other playhead
constexpr F32 kModDb = 1.f;                // slow level drift: +- this many dB ...
constexpr F32 kModMin = 10.f, kModMax = 30.f;     // ... re-aimed every kModMin..kModMax s, twice low-passed (kModTau)
constexpr F32 kModTau = 3.f;

// A decode on the General work queue. The worker owns its reference: shutdown just drops ours.
struct Decode
{
    std::vector<S16>  pcm;          // interleaved
    U32               rate = 0, channels = 0;
    std::string       err;
    bool              retryable = false;
    // #1012 (beds only, filled after the rotation): the coarse RMS envelope (full scale 1, one window
    // per envHop frames), its median, and for sparse beds the drop slices (mono, faded).
    bool              analyse = false, sparse = false;
    std::vector<F32>  env;
    U32               envHop = 0;
    F32               envMed = 0.f;
    std::vector<std::vector<S16>> drops;
    std::atomic<bool> done{false};
};

struct Layer
{
    enum EState : U8 { IDLE, LOADING, READY, FAILED };
    EState state = IDLE;
    U32    id = 0;                  // the engine's local loop
    std::shared_ptr<Decode> job;
    U64    bytes = 0;
    F32    secs = 0.f;
    F32    gain = 0.f;              // smoothed, before the master volume
    F32    silentFor = 0.f;
    F64    retryAt = 0.0;
    U32    tries = 0;
    // #1012 loop variation: the second playhead (a source over the same AL buffer; 0 = none),
    // the bed's envelope, the drop shots, and the state of the swaps / drift / drops.
    U32    id2 = 0;
    bool   noShare = false;         // the engine could not make a second playhead: single loop
    std::vector<F32> env;
    U32    envHop = 0, frames = 0, rate = 0;
    F32    envMed = 0.f;
    U32    drops[kMaxDrops] = {};
    U32    nDrops = 0, lastDrop = 0;
    U32    cur = 0;                 // the playhead that is (becoming) the heard one
    F32    xf = 1.f, xfDur = 3.f;   // cross-fade progress to `cur` (1: done) and its length
    F32    swapIn = 0.f;            // s of audible play until the next swap
    F32    modTarget = 0.f, mod1 = 0.f, mod2 = 0.f, modIn = 0.f;   // drift, in units of kModDb
    F32    dropIn = 0.f;
    bool   varInit = false;
};

// ---- thunder (slweatheraudio.h, THUNDER) ----
enum EThunder : U32 { TH_CLOSE = 0, TH_MID, TH_DISTANT, TH_CLASSES };
constexpr const char* kThunderPrefix[TH_CLASSES] = {"thunder_close_", "thunder_mid_", "thunder_distant_"};
constexpr U32 kThunderVariants[TH_CLASSES] = {5, 6, 8};   // pack 1.0.0: thunder_<class>_01..NN.ogg
constexpr U32 kThunderMaxVariants = 8;
constexpr U32 kThunderRecent = 2;            // a variant is not picked again within this many strikes of its class
constexpr F32 kCloseKm = 0.8f, kMidKm = 3.f; // class bounds
constexpr F32 kSoundSpeed = 343.f;           // m/s
constexpr F32 kDelayFull = 6000.f;           // m: real delay up to here, compressed beyond (WeatherThunderDelayScale)
constexpr F32 kMinDelay = 1.f;               // s: flash-to-bang is always felt
constexpr F32 kThunderPlaceMax = 120.f;      // m: placed at the strike's direction, this far at most
constexpr F32 kThunderOcc = 0.3f;            // x AudioAcousticsOcclusion: thunder diffracts around hills / houses
constexpr F32 kThunderIndoorHF = 0.12f;      // HF gain fully sheltered (the crack goes, the boom stays)
constexpr F32 kThunderIndoorLF = 0.6f;
constexpr F64 kThunderLate = 2.0;            // s: a thunder whose sound is not decoded this late is dropped
constexpr F64 kThunderFreeAfter = 60.0;      // s unused before a decoded variant is freed
constexpr size_t kMaxThunderVoices = 4;
constexpr F32 kDuck = 0.708f;                // close strike: the rain beds -3 dB ...
constexpr F32 kDuckAttack = 0.15f, kDuckRelease = 1.5f;   // ... 150 ms in, 1.5 s back out
constexpr F64 kDuckHold = 0.4;
// The acoustics source key for the thunder's occlusion ray (any fixed id: not an object).
const LLUUID kThunderKey("6d3c5b1e-0783-4c1a-9f00-7468756e6465");

struct ThunderVoice
{
    U32       strike = 0;    // SLWeather::Strike::serial
    U32       cls = 0, var = 0;
    F64       at = 0.0;      // when the sound starts (frame clock)
    F32       dir[3] = {0.f, 0.f, 1.f};
    F32       dist = 0.f;
    F32       pitch = 1.f;
    bool      started = false;
    bool      cut = false;   // #895: playing when the weather was reset (fades out, then stops)
};

struct Ctl
{
    LLAudioEngine* engine = nullptr;
    Layer layers[L_COUNT];
    // Thunder: decoded variants (the same lazy decode / free / budget as the beds; Layer::gain
    // and silentFor unused) and the voices waiting for their delay or playing.
    Layer shots[TH_CLASSES][kThunderMaxVariants];
    F64   shotUsed[TH_CLASSES][kThunderMaxVariants] = {};
    S32   recent[TH_CLASSES][kThunderRecent];
    std::vector<ThunderVoice> thunder;
    U32   lastStrike = 0;
    bool  strikesSeen = false;
    F32   duck = 0.f;
    F64   duckUntil = 0.0;
    Ctl() { for (auto& r : recent) for (S32& v : r) v = -1; }
    U32   inFlight = 0;
    U64   live = 0, peak = 0;       // decoded bytes handed to the engine
    bool  packChecked = false, packMissing = false;
    // Smoothed inputs.
    F32   rain = 0.f, snow = 0.f, speed = 0.f, wet = 0.f, muffle = 0.f, muffleTarget = 0.f;
    F32   runoff = 0.f;   // M4: run-off loudness near the listener (eased)
    F32   roofMat[LLCleanAcoustics::ROOF_MAT_COUNT] = {1.f, 0.f, 0.f, 0.f};
    // #894 openings: the placed beds' smoothed directions (agent axes, unit; zero: unplaced) and
    // distances, the whistle pitch, the log clock.
    LLVector3 smallDir, wideDir;
    F32   smallDist = 3.f, wideDist = 3.f;
    F32   pitch = 1.f;
    F64   nextOpenLog = 0.0;
    // #895: the weather reset last seen (SLWeather::resetSerial) and the cut it started.
    U32   resetSeen = 0;
    F64   cutUntil = 0.0;
    U32   seed = 0x9E3779B9u;
    F64   last = -1.0;
};

Ctl& ctl()
{
    static Ctl c;
    return c;
}

F32 smooth01(F32 e0, F32 e1, F32 x)
{
    const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

F32 follow(F32 tau, F32 dt) { return dt <= 0.f ? 0.f : 1.f - std::exp(-dt / tau); }

// Equal-power weights of three beds peaking at p[0] / p[1] / p[2] (alone below p[0] and above
// p[2]); sum of squares 1.
void bed3(F32 x, const F32 p[3], F32 w[3])
{
    const F32 t = llclamp((x - p[0]) / (p[1] - p[0]), 0.f, 1.f) + llclamp((x - p[1]) / (p[2] - p[1]), 0.f, 1.f);
    const F32 a = 1.5707963f * (t < 1.f ? t : t - 1.f);
    w[0] = w[1] = w[2] = 0.f;
    if (t < 1.f) { w[0] = std::cos(a); w[1] = std::sin(a); }
    else         { w[1] = std::cos(a); w[2] = std::sin(a); }
}

std::string bedPath(U32 l)
{
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "sounds", "weather", std::string(kLayers[l].name) + ".ogg");
}

// Off the main thread: the bed's coarse RMS envelope (start points) and, for sparse beds, the drop
// slices: the loudest transients of the bed itself, a short slice of each, mono, faded.
void analyseBed(Decode& d, U32 seed)
{
    const U32 ch = d.channels;
    const size_t frames = d.pcm.size() / ch;
    const size_t fw = std::max<size_t>(1, (size_t)(d.rate * kEnvFineSecs));
    const size_t nf = frames / fw;
    if (nf < 2 * kEnvCoarse) return;
    std::vector<F32> ms(nf);   // mean square per fine window (mono mean, full scale 1)
    for (size_t i = 0; i < nf; ++i)
    {
        F64 acc = 0.0;
        for (size_t f = i * fw; f < (i + 1) * fw; ++f)
        {
            S32 sum = 0;
            for (U32 k = 0; k < ch; ++k) sum += d.pcm[f * ch + k];
            const F64 v = (F64)sum / ((F64)ch * 32768.0);
            acc += v * v;
        }
        ms[i] = (F32)(acc / (F64)fw);
    }
    const size_t cw = nf / kEnvCoarse;
    d.envHop = (U32)(fw * kEnvCoarse);
    d.env.resize(cw);
    for (size_t w = 0; w < cw; ++w)
    {
        F32 acc = 0.f;
        for (U32 k = 0; k < kEnvCoarse; ++k) acc += ms[w * kEnvCoarse + k];
        d.env[w] = std::sqrt(acc / (F32)kEnvCoarse);
    }
    std::vector<F32> sorted(d.env);
    std::sort(sorted.begin(), sorted.end());
    d.envMed = sorted[sorted.size() / 2];
    if (!d.sparse) return;
    // Transients: local maxima of the 20 ms RMS above both the 90th percentile and 3x the median.
    std::vector<F32> fine(nf);
    for (size_t i = 0; i < nf; ++i) fine[i] = std::sqrt(ms[i]);
    std::vector<F32> fs(fine);
    std::sort(fs.begin(), fs.end());
    const F32 thr = std::max(fs[(size_t)((F32)nf * 0.9f)], 3.f * fs[nf / 2]);
    const size_t reach = (size_t)(0.24f / kEnvFineSecs);   // local maximum within +-240 ms
    std::vector<size_t> cand;
    for (size_t i = 0; i < nf; ++i)
    {
        if (fine[i] < thr) continue;
        bool top = true;
        for (size_t j = i > reach ? i - reach : 0; j <= std::min(nf - 1, i + reach) && top; ++j)
            top = fine[j] <= fine[i];
        if (top) cand.push_back(i);
    }
    std::sort(cand.begin(), cand.end(), [&fine](size_t a, size_t b) { return fine[a] > fine[b]; });
    const size_t sep = (size_t)(kDropMinSep / kEnvFineSecs);
    std::vector<size_t> pick;
    for (size_t i : cand)
    {
        bool spaced = true;
        for (size_t p : pick) spaced = spaced && (i > p ? i - p : p - i) >= sep;
        if (spaced) pick.push_back(i);
        if (pick.size() >= kMaxDrops) break;
    }
    for (size_t i : pick)
    {
        seed = seed * 1664525u + 1013904223u;
        const F32 len = kDropMin + (kDropMax - kDropMin) * ((F32)(seed >> 8) / 16777216.f);
        const size_t lead = (size_t)(kDropLead * d.rate);
        const size_t start = i * fw > lead ? i * fw - lead : 0;
        const size_t n = std::min((size_t)(len * d.rate), frames - start);
        if (n < 64) continue;
        std::vector<S16> sl(n);
        const size_t fin = std::max<size_t>(1, (size_t)(0.004f * d.rate)), fout = n * 4 / 10;
        for (size_t f = 0; f < n; ++f)
        {
            S32 sum = 0;
            for (U32 k = 0; k < ch; ++k) sum += d.pcm[(start + f) * ch + k];
            F32 v = (F32)sum / (F32)ch;
            if (f < fin) v *= (F32)f / (F32)fin;
            if (f + fout > n)
            {
                const F32 t = (F32)(n - f) / (F32)fout;
                v *= t * t * (3.f - 2.f * t);
            }
            sl[f] = (S16)v;
        }
        d.drops.push_back(std::move(sl));
    }
}

// Off the main thread: read (LLFile: wide-char paths on Windows), decode, start the loop at a
// random point (every bed loops over the whole file, so any rotation is seamless, and beds
// layered together never start in step).
void decodeJob(const std::string& path, U32 seed, const std::shared_ptr<Decode>& job, bool mono = false)
{
    try
    {
        const std::string data = LLFile::getContents(path);
        if (data.empty())
        {
            job->err = "missing or unreadable";
        }
        else
        {
            const int rc = llaudio_decode_ogg_pcm(reinterpret_cast<const uint8_t*>(data.data()), data.size(), job->pcm,
                                                  job->rate, job->channels, job->err);
            if (rc != LLAUDIO_DEC_OK)
            {
                job->err = "decode failed (" + job->err + ")";
                std::vector<S16>().swap(job->pcm);
            }
            else if (job->channels > 0)
            {
                if (mono && job->channels > 1)
                {
                    // #894: down to mono (a placed bed must be mono to pan): the channels' mean.
                    const U32 ch = job->channels;
                    const size_t fr = job->pcm.size() / ch;
                    for (size_t f = 0; f < fr; ++f)
                    {
                        S32 acc = 0;
                        for (U32 k = 0; k < ch; ++k) acc += job->pcm[f * ch + k];
                        job->pcm[f] = (S16)(acc / (S32)ch);
                    }
                    job->pcm.resize(fr);
                    std::vector<S16>(job->pcm).swap(job->pcm);   // (shrink: half the PCM)
                    job->channels = 1;
                }
                const size_t frames = job->pcm.size() / job->channels;
                if (frames > 1)
                    std::rotate(job->pcm.begin(), job->pcm.begin() + (seed % frames) * job->channels, job->pcm.end());
                if (job->analyse) analyseBed(*job, seed);
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        std::vector<S16>().swap(job->pcm);
        job->err = "out of memory";
        job->retryable = true;
    }
    catch (...)
    {
        std::vector<S16>().swap(job->pcm);
        job->err = "decode threw";
    }
    job->done = true;
}

void logLive(const Ctl& c, std::ostream& os)
{
    os << "; " << (c.live >> 20) << " MB live (peak " << (c.peak >> 20) << ", cap " << (kPcmBudget >> 20) << ")";
}

// What a ready bed holds besides its state (the engine drops the second playhead with the first).
void resetPlay(Layer& L)
{
    L.id = L.id2 = 0;
    L.nDrops = 0;
    std::fill(L.drops, L.drops + kMaxDrops, 0u);
    std::vector<F32>().swap(L.env);
    L.noShare = L.varInit = false;
    L.cur = 0;
    L.xf = 1.f;
    L.mod1 = L.mod2 = L.modTarget = 0.f;
}

void freeLayer(Ctl& c, U32 l)
{
    Layer& L = c.layers[l];
    if (L.state != Layer::READY) return;
    if (c.engine && c.engine == gAudiop)
    {
        for (U32 k = 0; k < L.nDrops; ++k) c.engine->destroyLocalLoop(L.drops[k]);
        if (L.id) c.engine->destroyLocalLoop(L.id);
    }
    c.live -= std::min(c.live, L.bytes);
    std::ostringstream os;
    os << "weather audio: freed " << kLayers[l].name;
    logLive(c, os);
    LL_INFOS("Weather") << os.str() << LL_ENDL;
    L.state = Layer::IDLE;
    resetPlay(L);
    L.bytes = 0;
    L.gain = 0.f;
    L.silentFor = 0.f;
}

// A finished decode -> a loop (main thread; the engine copies the PCM, ours is freed here).
void harvest(Ctl& c, U32 l, LLAudioEngine* eng, F64 now)
{
    Layer& L = c.layers[l];
    std::shared_ptr<Decode> job = std::move(L.job);
    --c.inFlight;
    L.state = Layer::IDLE;
    U32 id = 0;
    if (job->err.empty() && job->channels)
        id = eng->createLocalLoop(job->pcm.data(), (U32)(job->pcm.size() / job->channels), job->channels, job->rate);
    if (!id)
    {
        const bool retry = job->err.empty() || job->retryable;   // (engine refused: maybe out of AL memory)
        if (retry && ++L.tries < kMaxTries)
        {
            L.retryAt = now + kRetrySecs;
            LL_WARNS("Weather") << "weather audio: " << kLayers[l].name << ": "
                                << (job->err.empty() ? "the audio engine refused the loop" : job->err)
                                << "; retrying in " << kRetrySecs << " s" << LL_ENDL;
        }
        else
        {
            L.state = Layer::FAILED;   // (until restart: the layer stays silent)
            LL_WARNS("Weather") << "weather audio: " << kLayers[l].name << " (" << bedPath(l) << "): "
                                << (job->err.empty() ? "the audio engine refused the loop" : job->err)
                                << "; this layer stays silent" << LL_ENDL;
        }
        return;
    }
    L.state = Layer::READY;
    resetPlay(L);
    L.id = id;
    L.tries = 0;
    L.bytes = (U64)job->pcm.size() * sizeof(S16);
    L.secs = (F32)(job->pcm.size() / job->channels) / (F32)job->rate;
    L.gain = 0.f;
    L.silentFor = 0.f;
    // #1012: the envelope stays for the start points; the drop slices become engine shots.
    L.env = std::move(job->env);
    L.envHop = job->envHop;
    L.envMed = job->envMed;
    L.frames = (U32)(job->pcm.size() / job->channels);
    L.rate = job->rate;
    for (const std::vector<S16>& sl : job->drops)
    {
        const U32 sid = L.nDrops < kMaxDrops ? eng->createLocalShot(sl.data(), (U32)sl.size(), 1, job->rate) : 0;
        if (!sid) continue;
        L.drops[L.nDrops++] = sid;
        L.bytes += (U64)sl.size() * sizeof(S16);
    }
    c.live += L.bytes;
    c.peak = std::max(c.peak, c.live);
    std::ostringstream os;
    os << "weather audio: loaded " << kLayers[l].name << " (" << L.secs << " s, " << job->channels << " ch, "
       << job->rate << " Hz, " << (L.bytes >> 10) << " KB)";
    logLive(c, os);
    LL_INFOS("Weather") << os.str() << LL_ENDL;
    if (!L.env.empty())
    {
        std::vector<F32> e(L.env);
        std::sort(e.begin(), e.end());
        auto db = [](F32 v) { return 20.f * std::log10(std::max(v, 1e-6f)); };
        U32 loud = 0;
        for (F32 v : L.env) loud += v >= L.envMed ? 1u : 0u;
        std::ostringstream ev;
        ev << std::fixed << std::setprecision(1) << "weather audio: " << kLayers[l].name << " envelope " << L.env.size() << " x "
           << (F32)L.envHop / (F32)L.rate << " s, RMS dBFS p10 " << db(e[e.size() / 10]) << " median " << db(L.envMed) << " p90 "
           << db(e[e.size() * 9 / 10]) << ", " << loud << " start windows";
        if (kLayers[l].sparse)
        {
            ev << ", " << L.nDrops << " drop slices (";
            for (const std::vector<S16>& sl : job->drops) ev << (U32)(1000.f * (F32)sl.size() / (F32)job->rate) << " ";
            ev << "ms)";
        }
        LL_DEBUGS("Weather") << ev.str() << LL_ENDL;
    }
}

// ---- thunder variants ----
std::string shotName(U32 cls, U32 var)
{
    return llformat("%s%02u", kThunderPrefix[cls], var + 1);
}

bool shotBusy(const Ctl& c, U32 cls, U32 var)
{
    for (const ThunderVoice& v : c.thunder)
        if (v.cls == cls && v.var == var) return true;
    return false;
}

void freeShot(Ctl& c, U32 cls, U32 var)
{
    Layer& L = c.shots[cls][var];
    if (L.state != Layer::READY) return;
    if (c.engine && c.engine == gAudiop && L.id) c.engine->destroyLocalLoop(L.id);
    c.live -= std::min(c.live, L.bytes);
    std::ostringstream os;
    os << "weather audio: freed " << shotName(cls, var);
    logLive(c, os);
    LL_INFOS("Weather") << os.str() << LL_ENDL;
    L.state = Layer::IDLE;
    L.id = 0;
    L.bytes = 0;
}

// Room for `need` more decoded bytes: silent beds go first, then thunder no voice is using.
bool makeRoom(Ctl& c, U64 need, const F32 want[L_COUNT])
{
    for (U32 k = 0; k < L_COUNT && c.live + need > kPcmBudget; ++k)
        if (c.layers[k].state == Layer::READY && want[k] <= kSilent && c.layers[k].gain <= kSilent) freeLayer(c, k);
    for (U32 k = 0; k < TH_CLASSES && c.live + need > kPcmBudget; ++k)
        for (U32 v = 0; v < kThunderVariants[k] && c.live + need > kPcmBudget; ++v)
            if (!shotBusy(c, k, v)) freeShot(c, k, v);
    return c.live + need <= kPcmBudget;
}

// Queue a decode, within the budget: silent beds are freed to make room, else it waits.
bool startLoad(Ctl& c, U32 l, const F32 want[L_COUNT])
{
    if (c.inFlight >= kMaxInFlight) return false;
    if (!makeRoom(c, kInFlightGuess * (c.inFlight + 1), want)) return false;
    LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General");
    if (!q) return false;
    std::shared_ptr<Decode> job = std::make_shared<Decode>();
    c.seed = c.seed * 1664525u + 1013904223u;
    const U32 seed = c.seed ^ (U32)(LLFrameTimer::getTotalTime() & 0xffffffffu);
    const std::string path = bedPath(l);
    const bool mono = kLayers[l].mono;
    job->analyse = true;
    job->sparse = kLayers[l].sparse;
    if (!q->tryPost([path, seed, job, mono]() { decodeJob(path, seed, job, mono); })) return false;
    Layer& L = c.layers[l];
    L.state = Layer::LOADING;
    L.job = std::move(job);
    ++c.inFlight;
    return true;
}

// Inside a cross-fade group: what a not-yet-loaded bed wants goes to the nearest loaded one
// (power-preserving), so the bed that can be heard holds while the next one decodes.
void lendShares(const Ctl& c, const U32* group, U32 n, F32 apply[L_COUNT])
{
    for (U32 i = 0; i < n; ++i)
    {
        const U32 l = group[i];
        if (c.layers[l].state == Layer::READY || apply[l] <= 0.f) continue;
        S32 best = -1;
        for (U32 d = 1; d < n && best < 0; ++d)
        {
            if (i >= d && c.layers[group[i - d]].state == Layer::READY) best = (S32)group[i - d];
            else if (i + d < n && c.layers[group[i + d]].state == Layer::READY) best = (S32)group[i + d];
        }
        if (best >= 0) apply[best] = std::sqrt(apply[best] * apply[best] + apply[l] * apply[l]);
        apply[l] = 0.f;
    }
}

// A thunder variant's decode (from the start: no rotation).
bool startShotLoad(Ctl& c, U32 cls, U32 var, const F32 want[L_COUNT])
{
    // (One slot more than the beds: a storm starting with the rain must not wait for them.)
    if (c.inFlight >= kMaxInFlight + 1) return false;
    if (!makeRoom(c, kInFlightGuess * (c.inFlight + 1), want)) return false;
    LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General");
    if (!q) return false;
    std::shared_ptr<Decode> job = std::make_shared<Decode>();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "sounds", "weather", shotName(cls, var) + ".ogg");
    if (!q->tryPost([path, job]() { decodeJob(path, 0, job); })) return false;
    Layer& L = c.shots[cls][var];
    L.state = Layer::LOADING;
    L.job = std::move(job);
    ++c.inFlight;
    return true;
}

void harvestShot(Ctl& c, U32 cls, U32 var, LLAudioEngine* eng, F64 now)
{
    Layer& L = c.shots[cls][var];
    std::shared_ptr<Decode> job = std::move(L.job);
    --c.inFlight;
    L.state = Layer::IDLE;
    U32 id = 0;
    if (job->err.empty() && job->channels)
        id = eng->createLocalShot(job->pcm.data(), (U32)(job->pcm.size() / job->channels), job->channels, job->rate);
    if (!id)
    {
        const bool retry = job->err.empty() || job->retryable;
        if (retry && ++L.tries < kMaxTries)
            L.retryAt = now + kRetrySecs;
        else
            L.state = Layer::FAILED;
        LL_WARNS("Weather") << "weather audio: " << shotName(cls, var) << ": "
                            << (job->err.empty() ? "the audio engine refused the sound" : job->err)
                            << (L.state == Layer::FAILED ? "; not used again" : "; retrying later") << LL_ENDL;
        return;
    }
    L.state = Layer::READY;
    L.id = id;
    L.tries = 0;
    L.bytes = (U64)job->pcm.size() * sizeof(S16);
    L.secs = (F32)(job->pcm.size() / job->channels) / (F32)job->rate;
    c.shotUsed[cls][var] = now;
    c.live += L.bytes;
    c.peak = std::max(c.peak, c.live);
    std::ostringstream os;
    os << "weather audio: loaded " << shotName(cls, var) << " (" << L.secs << " s, " << job->channels << " ch, "
       << (L.bytes >> 10) << " KB)";
    logLive(c, os);
    LL_INFOS("Weather") << os.str() << LL_ENDL;
}

// Flash to bang: the sound-speed delay, compressed beyond kDelayFull, never under kMinDelay.
F32 thunderDelay(F32 dist)
{
    static LLCachedControl<F32> s_scale(gSavedSettings, "WeatherThunderDelayScale", 0.35f);
    const F32 full = std::min(dist, kDelayFull) / kSoundSpeed;
    const F32 rest = std::max(dist - kDelayFull, 0.f) / kSoundSpeed * llclamp((F32)s_scale, 0.f, 1.f);
    return std::max(full + rest, kMinDelay);
}

// A new strike -> a thunder voice: the class by distance, a variant of it not heard in the
// last kThunderRecent strikes of that class (and not broken), +-5 % pitch.
void queueThunder(Ctl& c, const SLWeather::Strike& s, F64 now)
{
    const F64 at = s.t0 + (F64)thunderDelay(s.dist);
    if (at < now - 0.5) return;   // (listed before this side started: long missed)
    const U32 cls = s.dist < kCloseKm * 1000.f ? TH_CLOSE : s.dist < kMidKm * 1000.f ? TH_MID : TH_DISTANT;
    U32 cand[kThunderMaxVariants];
    U32 n = 0;
    for (U32 v = 0; v < kThunderVariants[cls]; ++v)
    {
        bool recent = false;
        for (S32 r : c.recent[cls]) recent = recent || r == (S32)v;
        if (!recent && c.shots[cls][v].state != Layer::FAILED && !shotBusy(c, cls, v)) cand[n++] = v;
    }
    if (n == 0)
    {
        for (U32 v = 0; v < kThunderVariants[cls]; ++v)
            if (c.shots[cls][v].state != Layer::FAILED && !shotBusy(c, cls, v)) cand[n++] = v;
        if (n == 0) return;
    }
    ThunderVoice tv;
    tv.strike = s.serial;
    tv.cls = cls;
    tv.var = cand[s.hash % n];
    tv.at = at;
    tv.dist = s.dist;
    for (U32 i = 0; i < 3; ++i) tv.dir[i] = s.dir[i];
    tv.pitch = 1.f + 0.05f * ((F32)((s.hash >> 12) & 1023u) / 1023.f * 2.f - 1.f);
    for (U32 i = kThunderRecent - 1; i > 0; --i) c.recent[cls][i] = c.recent[cls][i - 1];
    c.recent[cls][0] = (S32)tv.var;
    if (c.thunder.size() >= kMaxThunderVoices)
    {
        // Full: the oldest waiting one gives way (a playing one finishes).
        auto it = std::find_if(c.thunder.begin(), c.thunder.end(), [](const ThunderVoice& v) { return !v.started; });
        if (it == c.thunder.end()) return;
        c.thunder.erase(it);
    }
    c.thunder.push_back(tv);
}

// ---- #1012 loop variation ----
F32 rnd01(Ctl& c)
{
    c.seed = c.seed * 1664525u + 1013904223u;
    return (F32)(c.seed >> 8) / 16777216.f;
}

// A start point for the idle playhead: a random loud window (envelope at or above the bed's
// median), not within kMinSepSecs of `other` (the heard playhead, frames; < 0: unknown).
U32 pickStart(Ctl& c, const Layer& L, S32 other)
{
    const U32 n = (U32)L.env.size();
    if (!n || !L.envHop) return (U32)(rnd01(c) * (F32)L.frames);
    const F32 sep = std::min(kMinSepSecs * (F32)L.rate, 0.2f * (F32)L.frames);
    auto farEnough = [&](U32 w)
    {
        if (other < 0) return true;
        const F32 centre = ((F32)w + 0.5f) * (F32)L.envHop;
        F32 d = std::fabs(centre - (F32)other);
        d = std::min(d, (F32)L.frames - d);
        return d >= sep;
    };
    for (S32 pass = 0; pass < 2; ++pass)   // (pass 1: any window if no loud one is far enough)
    {
        U32 count = 0;
        for (U32 w = 0; w < n; ++w)
            if ((pass == 1 || L.env[w] >= L.envMed) && farEnough(w)) ++count;
        if (!count) continue;
        U32 k = std::min((U32)(rnd01(c) * (F32)count), count - 1);
        for (U32 w = 0; w < n; ++w)
            if ((pass == 1 || L.env[w] >= L.envMed) && farEnough(w) && k-- == 0)
                return (U32)(((F32)w + rnd01(c)) * (F32)L.envHop) % L.frames;
    }
    return (U32)(rnd01(c) * (F32)L.frames);
}

// The two playheads' weights (equal power), the slow drift's gain. With `on` false nothing new
// starts (a fade under way finishes, no second playhead is made, the drift is off).
void variation(Ctl& c, Layer& L, LLAudioEngine* eng, F32 dt, bool on, F32& w0, F32& w1, F32& mul)
{
    w0 = 1.f; w1 = 0.f; mul = 1.f;
    if (on && !L.id2 && !L.noShare && L.frames)
    {
        L.id2 = eng->createLocalLoopShared(L.id);
        if (!L.id2) L.noShare = true;
    }
    if (!L.varInit)
    {
        L.varInit = true;
        L.swapIn = 5.f + (kSwapMax - 5.f) * rnd01(c);   // (the beds' first swaps are staggered)
        L.dropIn = 1.f + 2.f * rnd01(c);
        L.modIn = 0.f;
    }
    if (L.id2 && L.gain > kSilent)
    {
        if (L.xf < 1.f)
        {
            L.xf = std::min(L.xf + dt / L.xfDur, 1.f);
            if (L.xf >= 1.f) L.swapIn = kSwapMin + (kSwapMax - kSwapMin) * rnd01(c);
        }
        else if (on && (L.swapIn -= dt) <= 0.f)
        {
            const U32 curId = L.cur ? L.id2 : L.id, othId = L.cur ? L.id : L.id2;
            eng->seekLocalLoop(othId, pickStart(c, L, eng->localLoopPosition(curId)));
            L.cur = 1 - L.cur;   // (the new playhead is paused at weight 0 until the next frame)
            L.xf = 0.f;
            L.xfDur = kXfadeMin + (kXfadeMax - kXfadeMin) * rnd01(c);
        }
    }
    if (L.id2)
    {
        const F32 a = 1.5707963f * L.xf;
        const F32 wn = L.xf >= 1.f ? 1.f : std::sin(a), wo = L.xf >= 1.f ? 0.f : std::cos(a);
        w0 = L.cur ? wo : wn;
        w1 = L.cur ? wn : wo;
    }
    if (on)
    {
        if ((L.modIn -= dt) <= 0.f)
        {
            L.modTarget = 2.f * rnd01(c) - 1.f;
            L.modIn = kModMin + (kModMax - kModMin) * rnd01(c);
        }
        const F32 k = follow(kModTau, dt);
        L.mod1 += (L.modTarget - L.mod1) * k;
        L.mod2 += (L.mod1 - L.mod2) * k;
        mul = std::exp(L.mod2 * kModDb * 0.1151293f);
    }
}

// Sparse beds: a random one-shot drop (one of the bed's own transients) at a small random place
// around the listener, at a Poisson-ish rate that follows the bed's level.
void dropStep(Ctl& c, Layer& L, LLAudioEngine* eng, F32 dt, F32 vol, F32 duckMul)
{
    if ((L.dropIn -= dt) > 0.f) return;
    const F32 rate = kDropRate * std::min(L.gain, 1.f);
    L.dropIn = llclamp(-std::log(std::max(1.f - rnd01(c), 1e-3f)) / std::max(rate, 1e-3f), 0.1f, 3.f);
    const U32 first = std::min((U32)(rnd01(c) * (F32)L.nDrops), L.nDrops - 1);
    for (U32 k = 0; k < L.nDrops; ++k)
    {
        const U32 i = (first + k) % L.nDrops;
        if ((L.nDrops > 1 && i == L.lastDrop) || eng->localShotPlaying(L.drops[i])) continue;
        const F32 az = 6.2831853f * rnd01(c), el = -0.2f + 0.9f * rnd01(c);
        const F32 dist = kDropDistMin + (kDropDistMax - kDropDistMin) * rnd01(c);
        const LLVector3 off(std::cos(el) * std::cos(az) * dist, std::cos(el) * std::sin(az) * dist, std::sin(el) * dist);
        const F32 g = vol * kDropGain * L.gain * duckMul * (0.5f + 0.5f * rnd01(c));
        eng->setLocalShot(L.drops[i], off, g, 1.f, 1.f, 0.f);
        eng->playLocalShot(L.drops[i], off, g, 1.f + 0.06f * (2.f * rnd01(c) - 1.f));
        L.lastDrop = i;
        break;
    }
}
} // namespace

void SLWeatherAudio::update()
{
    Ctl& c = ctl();
    LLAudioEngine* eng = gAudiop;
    if (eng != c.engine)
    {
        // The engine was replaced or is gone: its loops went with it (decodes still finish).
        for (Layer& L : c.layers)
            if (L.state == Layer::READY) { L.state = Layer::IDLE; resetPlay(L); L.bytes = 0; L.gain = 0.f; }
        for (auto& cls : c.shots)
            for (Layer& L : cls)
                if (L.state == Layer::READY) { L.state = Layer::IDLE; L.id = 0; L.bytes = 0; }
        c.thunder.clear();
        c.live = 0;
        c.engine = eng;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const F32 dt = c.last < 0.0 ? 0.f : (F32)llclamp(now - c.last, 0.0, 0.5);
    c.last = now;

    static LLCachedControl<bool> s_on(gSavedSettings, "WeatherAudio", true);
    static LLCachedControl<bool> s_var(gSavedSettings, "WeatherAudioLoopVariation", true);
    static LLCachedControl<F32>  s_vol(gSavedSettings, "WeatherAudioVolume", 0.5f);
    static LLCachedControl<S32>  s_ear(gSavedSettings, "MediaSoundsEarLocation", 0);

    SLWeather& wx = SLWeather::instance();
    const SLWeather::State& st = wx.state();
    // #895 / #904 CUT: the weather was reset (master off, a teleport out of the region / parcel
    // weather). Nothing of the old place's weather may stay audible: the smoothed inputs drop to
    // zero (the beds fall with kCutRelease, the openings stop asking for room rays), thunder not
    // yet started is dropped, thunder playing fades over kCutSecs and stops, the duck ends.
    if (wx.resetSerial() != c.resetSeen)
    {
        c.resetSeen = wx.resetSerial();
        c.rain = c.snow = c.speed = c.wet = c.runoff = 0.f;
        c.muffleTarget = 0.f;
        c.duck = 0.f;
        c.duckUntil = 0.0;
        size_t dropped = 0, fading = 0;
        for (auto it = c.thunder.begin(); it != c.thunder.end();)
        {
            if (!it->started) { it = c.thunder.erase(it); ++dropped; continue; }
            it->cut = true;
            ++fading;
            ++it;
        }
        if (!wx.strikes().empty()) c.lastStrike = wx.strikes().back().serial;
        c.cutUntil = now + kCutSecs;
        LLCleanAcoustics::cancelOpenings();   // (#894: no more lite room rays for the old wind)
        LL_INFOS("Weather") << "weather audio: weather reset -- beds cut, " << dropped << " queued thunder dropped, " << fading
                            << " fading" << LL_ENDL;
    }
    const bool cutting = now < c.cutUntil;
    // Nothing to hear and nothing alive: no work at all.
    bool alive = !c.thunder.empty();
    for (const Layer& L : c.layers) alive = alive || L.state == Layer::READY || L.state == Layer::LOADING;
    for (const auto& cls : c.shots)
        for (const Layer& L : cls) alive = alive || L.state == Layer::READY || L.state == Layer::LOADING;
    const bool newStrike = !wx.strikes().empty() && wx.strikes().back().serial != c.lastStrike;
    if (!alive && (!s_on || (!st.visible() && !newStrike)) && c.rain <= kSilent && c.snow <= kSilent && c.wet <= kSilent)
    {
        if (!wx.strikes().empty()) c.lastStrike = wx.strikes().back().serial;   // (off: not heard later either)
        return;
    }

    // Where the renderer reads the roof map: the listener (the OpenAL ear,
    // MediaSoundsEarLocation), relative to the camera.
    const LLVector3d cam = gAgentCamera.getCameraPositionGlobal();
    const LLVector3d earGlobal = s_ear == 1 ? gAgent.getPositionGlobal() : cam;
    {
        const LLVector3d d = earGlobal - cam;
        wx.setEarOffset((F32)d.mdV[VX], (F32)d.mdV[VY], (F32)d.mdV[VZ]);
    }

    // Inputs (eased): rain / snow amounts, wind speed, surface wetness (drips drain it).
    F32 rainT = 0.f, snowT = 0.f, speedT = 0.f, wetT = 0.f;
    if (s_on && eng)
    {
        rainT = llclamp(st.rain, 0.f, 1.f);
        snowT = llclamp(st.snow, 0.f, 1.f);
        speedT = std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]);
        wetT = llclamp(st.wetness, 0.f, 1.f);
    }
    // Cover: the room acoustics' measured roof when it has one (a porch with open sides counts
    // less than a closed room), else the roof map at the listener (no answer: keep the last).
    F32 enclosure = 0.f, roof = 0.f;
    bool coverLive = true;
    if (LLCleanAcoustics::listenerRoom(enclosure, roof))
        c.muffleTarget = llclamp(roof, 0.f, 1.f) * (0.6f + 0.4f * llclamp(enclosure, 0.f, 1.f));
    else if (wx.listenerOpen() >= 0.f)
        c.muffleTarget = 1.f - llclamp(wx.listenerOpen(), 0.f, 1.f);
    else
        coverLive = false;   // (the roof map is only read while precipitation simulates)
    // What the roof is made of (held while unmeasured; all wood until then).
    LLCleanAcoustics::listenerRoofMaterial(c.roofMat);
    const bool underwater = LLViewerCamera::getInstance()->cameraUnderWater();
    const F32 muffleT = underwater ? 1.f : c.muffleTarget;

    const F32 kl = follow(kLevelTau, dt);
    c.rain += (rainT - c.rain) * kl;
    c.snow += (snowT - c.snow) * kl;
    c.speed += (speedT - c.speed) * follow(kSpeedTau, dt);
    c.wet += (wetT - c.wet) * kl;
    c.muffle += (muffleT - c.muffle) * follow(kMuffleTau, dt);
    if (c.rain < kSilent && rainT <= 0.f) c.rain = 0.f;
    if (c.snow < kSilent && snowT <= 0.f) c.snow = 0.f;
    if (c.wet < kSilent && wetT <= 0.f) c.wet = 0.f;
    // M4: run-off near the listener (the renderer's readback: drips / streams within ~12 m).
    const F32 runoffT = (s_on && eng) ? llclamp(wx.runoffNear(), 0.f, 1.f) : 0.f;
    c.runoff += (runoffT - c.runoff) * kl;
    if (c.runoff < kSilent && runoffT <= 0.f) c.runoff = 0.f;

    // ---- what each layer wants (before the master volume; see slweatheraudio.h) ----
    F32 want[L_COUNT] = {};
    const F32 m = llclamp(c.muffle, 0.f, 1.f);
    const F32 open = std::sqrt(1.f - m), cov = std::sqrt(m);
    const F32 rainLvl = smooth01(0.f, 0.12f, c.rain) * (1.f + kDownpourLift * smooth01(0.75f, 1.f, c.rain));   // (#954)
    const F32 snowLvl = smooth01(0.f, 0.1f, c.snow);
    const F32 hailLvl = 0.f;   // no hail in the weather state yet: the hail beds stay unloaded
    {
        F32 w[3];
        bed3(c.rain, kRainPeak, w);
        for (U32 i = 0; i < 3; ++i) want[kGroupRain[i]] = rainLvl * open * w[i];
        // Roof: each material by its share (equal power), its heavy variant crossing in. Shares
        // under kMinRoofShare are dropped first (a stray ray must not load a whole bed).
        F32 sh[LLCleanAcoustics::ROOF_MAT_COUNT];
        F32 shSum = 0.f;
        for (U32 k = 0; k < LLCleanAcoustics::ROOF_MAT_COUNT; ++k) shSum += (sh[k] = std::max(c.roofMat[k] - kMinRoofShare, 0.f));
        for (U32 k = 0; k < LLCleanAcoustics::ROOF_MAT_COUNT; ++k)
            sh[k] = shSum > 0.f ? sh[k] / shSum : (k == LLCleanAcoustics::ROOF_WOOD ? 1.f : 0.f);
        const F32 q = 1.5707963f * smooth01(kRoofHeavy0, kRoofHeavy1, c.rain);
        const F32 roofLvl = rainLvl * cov;
        want[ROOF_WOOD] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_WOOD]) * std::cos(q);
        want[ROOF_WOOD_HEAVY] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_WOOD]) * std::sin(q);
        want[ROOF_METAL] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_METAL]) * std::cos(q);
        want[ROOF_METAL_HEAVY] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_METAL]) * std::sin(q);
        want[ROOF_GLASS] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_GLASS]);
        want[ROOF_TENT] = roofLvl * std::sqrt(sh[LLCleanAcoustics::ROOF_TENT]);
        // Drips: under cover while it rains, and on while the surfaces drain -- the draining
        // part only with a live cover answer (after the rain the roof map is not read any
        // more: a held "covered" would follow you out into the open).
        want[DRIP] = kDripGain * cov * smooth01(0.1f, 0.6f, coverLive ? std::max(c.rain, c.wet) : c.rain);
        // M4 RUN-OFF: an eave dripping or a stream pouring near the listener, in the open too
        // (the same drip bed; its level by the run-off's loudness there).
        want[DRIP] = std::max(want[DRIP], kRunoffDripGain * smooth01(0.05f, 0.6f, c.runoff));
        // Hail: outdoors like the rain; on the roof hard materials ring more than soft ones.
        const F32 hard = sh[LLCleanAcoustics::ROOF_METAL] + sh[LLCleanAcoustics::ROOF_GLASS];
        want[HAIL_OUTDOOR] = hailLvl * open;
        want[HAIL_ROOF] = hailLvl * cov * std::sqrt(hard + kHailRoofSoft * (1.f - hard));
    }
    {
        // Wind: only with precipitation (it is the storm's sound, not the region's wind), quiet
        // in snow; indoors the indoor bed (in snow the muffled winter wind) by its strength.
        const F32 precip = c.rain + c.snow;
        const F32 snowShare = precip > 1e-4f ? c.snow / precip : 0.f;
        // (#954: blowing snow counts as precipitation for the wind -- a ground blizzard howls too)
        const F32 windLvl = smooth01(1.f, 5.f, c.speed) * smooth01(0.f, 0.15f, std::max(precip, st.blowing));
        F32 w[3];
        bed3(c.speed, kWindPeak, w);
        // #954 BLIZZARD: heavy snow in a strong wind is loud, not muffled.
        const F32 bliz = smooth01(0.7f, 0.9f, std::max(c.snow, st.blowing)) * smooth01(8.f, 16.f, c.speed);
        const F32 outdoor = windLvl * open * (1.f + (kSnowWind - 1.f) * snowShare * (1.f - bliz)) * (1.f + kBlizzardLift * bliz);
        for (U32 i = 0; i < 3; ++i) want[kGroupWind[i]] = outdoor * w[i];
        const F32 indoor = windLvl * cov * smooth01(2.f, 12.f, c.speed);
        want[WIND_INDOOR] = indoor * std::sqrt(1.f - snowShare);
        want[SNOW_WIND_MUFFLED] = indoor * std::sqrt(snowShare);
        // #894 WIND THROUGH OPENINGS (slweatheraudio.h): under cover, the openings around the
        // listener (the acoustics room rays: LLCleanAcoustics::listenerOpenings) shape the wind
        // -- level from the open area facing into the wind, tone by size (cracks whistle, small
        // openings a lower whistle, window-sized ones howl, wide ones a broadband rush), placed at
        // the dominant opening, gusts lifting the level and the whistle's pitch. No rays: the
        // roof map's openness at the listener as one wide, unplaced opening.
        static LLCachedControl<bool> s_openings(gSavedSettings, "WeatherAudioWindOpenings", true);
        F32 crack = 0.f, small = 0.f, window = 0.f, wide = 0.f;
        LLVector3 smallDir, wideDir;
        F32 smallBest = 0.f, wideBest = 0.f, smallDist = 3.f, wideDist = 3.f;
        bool rays = false;
        U32 nOpen = 0;
        LLCleanAcoustics::Opening ops[LLCleanAcoustics::kMaxOpenings];
        F32 facing[LLCleanAcoustics::kMaxOpenings] = {};
        if (s_openings && windLvl > kSilent && c.muffleTarget > 0.1f)
        {
            LLCleanAcoustics::wantOpenings();   // (keeps the room rays coming; nothing without ray tracing)
            F32 esc = 0.f;
            rays = LLCleanAcoustics::listenerOpenings(ops, nOpen, esc);
            const F32 wl = std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]);
            const F32 intoX = wl > 0.1f ? -st.wind[0] / wl : 0.f, intoY = wl > 0.1f ? -st.wind[1] / wl : 0.f;
            for (U32 k = 0; rays && k < nOpen; ++k)
            {
                const LLCleanAcoustics::Opening& op = ops[k];
                const F32 hl = std::sqrt(op.dir.mV[VX] * op.dir.mV[VX] + op.dir.mV[VY] * op.dir.mV[VY]);
                const F32 dotW = hl > 1e-3f ? (op.dir.mV[VX] * intoX + op.dir.mV[VY] * intoY) / hl : 0.f;
                const F32 horiz = (kOpenLee + (1.f - kOpenLee) * smooth01(-0.3f, 0.8f, dotW));
                const F32 f = kOpenUp + (horiz - kOpenUp) * smooth01(0.2f, 0.7f, hl);
                facing[k] = f;
                const F32 e = op.solid * f;   // windward solid angle (sr)
                const F32 x = op.solid;
                const F32 wCrack = 1.f - smooth01(kOpenCrack0, kOpenCrack1, x);
                const F32 wSmall = smooth01(kOpenCrack0, kOpenCrack1, x) * (1.f - smooth01(kOpenSmall0, kOpenSmall1, x));
                const F32 wWide = smooth01(kOpenWide0, kOpenWide1, x);
                const F32 wWindow = std::max(1.f - wCrack - wSmall - wWide, 0.f);
                crack += e * wCrack; small += e * wSmall; window += e * wWindow; wide += e * wWide;
                if (e * (wCrack + wSmall) > smallBest) { smallBest = e * (wCrack + wSmall); smallDir = op.dir; smallDist = op.dist; }
                if (e * (wWindow + wWide) > wideBest) { wideBest = e * (wWindow + wWide); wideDir = op.dir; wideDist = op.dist; }
            }
            if (!rays && wx.listenerOpen() >= 0.f)
                wide = kOpenFallbackSr * llclamp(wx.listenerOpen(), 0.f, 1.f) * kOpenUp;
        }
        // Gusts: the gusty wind against its mean (lifts the openings' level and the whistle's pitch).
        const F32 meanSp = std::sqrt(st.windMean[0] * st.windMean[0] + st.windMean[1] * st.windMean[1]);
        const F32 gust = llclamp((std::sqrt(st.wind[0] * st.wind[0] + st.wind[1] * st.wind[1]) - meanSp) / std::max(meanSp, 1.f), -0.5f, 1.f);
        const F32 gm = llclamp(1.f + 0.6f * gust, 0.4f, 1.6f);
        const F32 through = windLvl * cov * std::sqrt(1.f - 0.6f * snowShare);   // (snow: quieter)
        const F32 strong = smooth01(4.f, 12.f, c.speed);
        want[WIND_WHISTLE] = std::min(through * strong * gm * smooth01(0.f, kOpenFullCrack, crack), 1.f);
        want[WIND_WHISTLE_B] = std::min(through * strong * gm * smooth01(0.f, kOpenFullSmall, small), 1.f);
        want[WIND_HOWL_WINDOW] = std::min(through * gm * smooth01(6.f, 14.f, c.speed) * smooth01(0.f, kOpenFullWindow, window), 1.f);
        const F32 rush = std::min(through * gm * smooth01(0.f, kOpenFullWide, wide), 1.f);
        const F32 rq = 1.5707963f * smooth01(4.f, 10.f, c.speed);
        want[RUSH_LIGHT] = rush * std::cos(rq);
        want[RUSH_GUST] = rush * std::sin(rq);
        // The sealed-room bed gives way to the openings as they grow (a sealed room: unchanged).
        const F32 expo = smooth01(0.f, 1.f, crack + small + window + wide);
        want[WIND_INDOOR] *= 1.f - kOpenIndoorDuck * expo;
        want[SNOW_WIND_MUFFLED] *= 1.f - kOpenIndoorDuck * expo;
        // Placement: the dominant small / wide opening (eased), at the wall it sits in.
        const F32 kd = follow(kOpenDirTau, dt);
        auto ease = [kd](LLVector3& cur, const LLVector3& tgt)
        {
            if (tgt.isExactlyZero()) { cur.clear(); return; }
            if (cur.isExactlyZero()) { cur = tgt; return; }
            LLVector3 v = cur + (tgt - cur) * kd;
            cur = v.normalize() > 1e-3f ? v : tgt;
        };
        ease(c.smallDir, smallBest > 0.f ? smallDir : LLVector3::zero);
        ease(c.wideDir, wideBest > 0.f ? wideDir : LLVector3::zero);
        c.smallDist += (smallDist - c.smallDist) * kd;
        c.wideDist += (wideDist - c.wideDist) * kd;
        c.pitch += (llclamp(1.f + kWhistlePitchGust * gust, 0.85f, 1.25f) - c.pitch) * follow(kSpeedTau, dt);
        if ((want[WIND_WHISTLE] + want[WIND_WHISTLE_B] + want[WIND_HOWL_WINDOW] + rush) > kLoadAt && now >= c.nextOpenLog)
        {
            c.nextOpenLog = now + 10.0;
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << "weather wind openings: " << (rays ? "rays" : "roof map") << ", " << nOpen
               << " opening(s)";
            for (U32 k = 0; rays && k < nOpen; ++k)
                os << " [" << ops[k].solid << " sr, windward " << facing[k] << ", dir " << ops[k].dir.mV[VX] << " "
                   << ops[k].dir.mV[VY] << " " << ops[k].dir.mV[VZ] << ", " << ops[k].dist << " m]";
            os << "; windward sr crack " << crack << " small " << small << " window " << window << " wide " << wide << "; whistle "
               << want[WIND_WHISTLE] << " / " << want[WIND_WHISTLE_B] << " (pitch " << c.pitch << "), howl " << want[WIND_HOWL_WINDOW]
               << ", rush " << rush << ", sealed-room bed x" << (1.f - kOpenIndoorDuck * expo) << ", gust " << gust;
            LL_INFOS("Weather") << os.str() << LL_ENDL;
        }
    }
    want[SNOW_AMBIENCE] = snowLvl * open;
    want[SNOW_INTERIOR] = snowLvl * cov;

    // ---- load what is wanted (loudest first), harvest finished decodes ----
    bool wanted = false;
    for (U32 l = 0; l < L_COUNT; ++l) wanted = wanted || want[l] > kLoadAt;
    if ((wanted || newStrike || !c.thunder.empty()) && !c.packChecked)
    {
        c.packChecked = true;
        const std::string dir = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "sounds", "weather");
        c.packMissing = !LLFile::isdir(dir);
        if (c.packMissing)
            LL_WARNS("Weather") << "weather sound pack not found (" << dir << "): weather audio stays silent" << LL_ENDL;
    }
    if (eng)   // (no engine: a finished decode waits for one)
        for (U32 l = 0; l < L_COUNT; ++l)
            if (c.layers[l].state == Layer::LOADING && c.layers[l].job->done) harvest(c, l, eng, now);
    if (eng && !c.packMissing)
    {
        U32 order[L_COUNT];
        for (U32 l = 0; l < L_COUNT; ++l) order[l] = l;
        std::sort(order, order + L_COUNT, [&want](U32 a, U32 b) { return want[a] > want[b]; });
        for (U32 l : order)
        {
            const Layer& L = c.layers[l];
            if (want[l] <= kLoadAt) break;
            if (L.state != Layer::IDLE || now < L.retryAt) continue;
            if (!startLoad(c, l, want)) break;
        }
    }

    // ---- thunder (slweatheraudio.h, THUNDER) ----
    const F32 vol = eng ? llclamp((F32)s_vol, 0.f, 1.f) * eng->getSecondaryGain(LLAudioEngine::AUDIO_TYPE_AMBIENT)
                              * (underwater ? kUnderwaterGain : 1.f)
                        : 0.f;
    if (eng)
    {
        for (U32 k = 0; k < TH_CLASSES; ++k)
            for (U32 v = 0; v < kThunderVariants[k]; ++v)
                if (c.shots[k][v].state == Layer::LOADING && c.shots[k][v].job->done) harvestShot(c, k, v, eng, now);
    }
    // New strikes (each once).
    for (const SLWeather::Strike& s : wx.strikes())
    {
        if (c.strikesSeen && (S32)(s.serial - c.lastStrike) <= 0) continue;
        if (s_on && eng && !c.packMissing) queueThunder(c, s, now);
    }
    if (!wx.strikes().empty())
    {
        c.lastStrike = wx.strikes().back().serial;
        c.strikesSeen = true;
    }
    // Decode what the voices will need (a strike is listed SLWeather::kLookahead before its
    // flash, and the bang comes >= 1 s after that: time enough for a decode).
    if (eng && !c.packMissing)
        for (const ThunderVoice& tv : c.thunder)
        {
            const Layer& L = c.shots[tv.cls][tv.var];
            if (L.state == Layer::IDLE && now >= L.retryAt && !startShotLoad(c, tv.cls, tv.var, want)) break;
        }
    // Start, follow, end.
    {
        // Sheltering: the weather cover (eased), the room's enclosure for the reverb send.
        F32 encl = m, roofIgnored = 0.f;
        LLCleanAcoustics::listenerRoom(encl, roofIgnored);
        static LLCachedControl<F32> s_occ(gSavedSettings, "AudioAcousticsOcclusion", 0.8f);
        F32 occ = 0.f;
        LLCleanAcoustics::occlusionOf(kThunderKey, occ);
        const F32 strength = kThunderOcc * llclamp((F32)s_occ, 0.f, 1.f);
        const F32 directGain = (1.f + (kThunderIndoorLF - 1.f) * m) * llclamp(1.f - strength * 0.55f * occ, 0.2f, 1.f);
        const F32 directHF = (1.f + (kThunderIndoorHF - 1.f) * m) * llclamp(1.f - strength * 0.9f * occ, 0.05f, 1.f);
        const F32 send = smooth01(0.2f, 0.9f, encl);
        const F32 gain = s_on ? vol : 0.f;
        const ThunderVoice* traced = nullptr;
        for (auto it = c.thunder.begin(); it != c.thunder.end();)
        {
            ThunderVoice& tv = *it;
            Layer& L = c.shots[tv.cls][tv.var];
            const F32 place = std::min(tv.dist, kThunderPlaceMax);
            const LLVector3 offset(tv.dir[0] * place, tv.dir[1] * place, tv.dir[2] * place);
            if (tv.cut && !cutting)
            {
                // (faded out: stopped now -- freeing the variant stops its source)
                const U32 cls = tv.cls, var = tv.var;
                it = c.thunder.erase(it);
                if (!shotBusy(c, cls, var)) freeShot(c, cls, var);
                continue;
            }
            if (!tv.started)
            {
                if (now < tv.at) { ++it; continue; }
                if (!eng || L.state == Layer::FAILED || now > tv.at + kThunderLate)
                {
                    LL_INFOS("Weather") << "weather audio: " << shotName(tv.cls, tv.var) << " dropped ("
                                        << (L.state == Layer::FAILED ? "unusable" : "not decoded in time") << ")" << LL_ENDL;
                    it = c.thunder.erase(it);
                    continue;
                }
                if (L.state != Layer::READY) { ++it; continue; }   // (still decoding: a little late)
                eng->setLocalShot(L.id, offset, gain, directGain, directHF, send);
                eng->playLocalShot(L.id, offset, gain, tv.pitch);
                tv.started = true;
                c.shotUsed[tv.cls][tv.var] = now;
                if (tv.cls == TH_CLOSE) c.duckUntil = now + kDuckHold;
                LL_DEBUGS("Weather") << "thunder " << shotName(tv.cls, tv.var) << " at " << tv.dist << " m, pitch " << tv.pitch
                                     << LL_ENDL;
            }
            else if (!eng || L.state != Layer::READY || !eng->localShotPlaying(L.id))
            {
                it = c.thunder.erase(it);
                continue;
            }
            else
            {
                const F32 fade = tv.cut ? (F32)llclamp((c.cutUntil - now) / kCutSecs, 0.0, 1.0) : 1.f;
                eng->setLocalShot(L.id, offset, gain * fade, directGain, directHF, send);
                c.shotUsed[tv.cls][tv.var] = now;
            }
            traced = &tv;   // (the newest playing one gets the occlusion ray)
            ++it;
        }
        if (traced)
        {
            const F32 place = std::min(traced->dist, kThunderPlaceMax);
            const LLVector3 ear = gAgent.getPosAgentFromGlobal(earGlobal);
            LLCleanAcoustics::traceExtraSource(kThunderKey, ear + LLVector3(traced->dir[0] * place, traced->dir[1] * place,
                                                                           traced->dir[2] * place));
        }
    }
    // Decoded variants nobody used for a while go.
    for (U32 k = 0; k < TH_CLASSES; ++k)
        for (U32 v = 0; v < kThunderVariants[k]; ++v)
            if (c.shots[k][v].state == Layer::READY && now - c.shotUsed[k][v] > kThunderFreeAfter && !shotBusy(c, k, v))
                freeShot(c, k, v);
    // Close strike: the rain beds duck briefly.
    c.duck += ((now < c.duckUntil ? 1.f : 0.f) - c.duck) * follow(now < c.duckUntil ? kDuckAttack : kDuckRelease, dt);
    if (c.duck < kSilent && now >= c.duckUntil) c.duck = 0.f;
    const F32 duckMul = 1.f + (kDuck - 1.f) * c.duck;

    // ---- apply ----
    F32 apply[L_COUNT];
    std::copy(want, want + L_COUNT, apply);
    lendShares(c, kGroupRain, LL_ARRAY_SIZE(kGroupRain), apply);
    lendShares(c, kGroupRoof, LL_ARRAY_SIZE(kGroupRoof), apply);
    lendShares(c, kGroupWind, LL_ARRAY_SIZE(kGroupWind), apply);
    lendShares(c, kGroupRush, LL_ARRAY_SIZE(kGroupRush), apply);
    const F32 ka = follow(kAttack, dt), kr = follow(cutting ? kCutRelease : kRelease, dt);
    for (U32 l = 0; l < L_COUNT; ++l)
    {
        Layer& L = c.layers[l];
        if (L.state != Layer::READY) continue;
        const F32 t = apply[l];
        L.gain += (t - L.gain) * (t > L.gain ? ka : kr);
        if (t <= 0.f && L.gain < kSilent) L.gain = 0.f;
        // (The duck acts on the level sent, not on the eased gain: it must be quick.)
        const bool ducks = l <= DRIP;   // rain, roof and drip beds
        const bool whistle = l == WIND_WHISTLE || l == WIND_WHISTLE_B;
        const F32 level = vol * kLayers[l].gain * L.gain * (ducks ? duckMul : 1.f), pit = whistle ? c.pitch : 1.f;
        // #1012: two playheads cross-fading to fresh start points, the slow drift (WeatherAudioLoopVariation).
        F32 w0 = 1.f, w1 = 0.f, mul = 1.f;
        if (s_var || L.id2) variation(c, L, eng, dt, s_var, w0, w1, mul);
        eng->setLocalLoop(L.id, level * mul * w0, pit);   // (gain 0: paused)
        if (L.id2) eng->setLocalLoop(L.id2, level * mul * w1, pit);
        if (kLayers[l].placed)
        {
            // #894: at the dominant opening of its class (unplaced: at the listener).
            const LLVector3& d = whistle ? c.smallDir : c.wideDir;
            const F32 at = llclamp(whistle ? c.smallDist : c.wideDist, kOpenPlaceMin, kOpenPlaceMax);
            eng->setLocalLoopOffset(L.id, d * at);
            if (L.id2) eng->setLocalLoopOffset(L.id2, d * at);
        }
        if (s_var && kLayers[l].sparse && L.nDrops && L.gain > kSilent) dropStep(c, L, eng, dt, vol, ducks ? duckMul : 1.f);
        if (L.gain > 0.f || want[l] > kSilent)
        {
            L.silentFor = 0.f;
            continue;
        }
        L.silentFor += dt;
        if (L.silentFor > kFreeAfter) freeLayer(c, l);
    }
}

void SLWeatherAudio::shutdown()
{
    Ctl& c = ctl();
    for (U32 l = 0; l < L_COUNT; ++l) freeLayer(c, l);
    c.thunder.clear();
    for (U32 k = 0; k < TH_CLASSES; ++k)
        for (U32 v = 0; v < kThunderVariants[k]; ++v) freeShot(c, k, v);
    for (Layer& L : c.layers)
    {
        // A decode still on the work queue keeps its own reference and finishes into it; the
        // General pool is closed (and joined) after the audio engine.
        L.job.reset();
        L = Layer();
    }
    for (auto& cls : c.shots)
        for (Layer& L : cls)
        {
            L.job.reset();
            L = Layer();
        }
    c.duck = 0.f;
    c.inFlight = 0;
    c.live = 0;
    c.engine = nullptr;
    c.rain = c.snow = c.speed = c.wet = c.muffle = 0.f;
}
