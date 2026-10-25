/**
 * @file slweather.h
 * @brief Weather state (#783): precipitation, wind, fog and surface wetness with smooth
 *        transitions, resolved from layered sources. CPU only -- the Vulkan renderer draws
 *        it (vkgfx/llcleanweathervk.h).
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
 *
 * SOURCES, highest priority first (resolve()):
 *   MASTER  WeatherMasterEnabled off: every source below is ignored (see masterEnabled()).
 *           #904: and every accumulated amount (snow cover / depth, wetness, run-off, wind,
 *           a flash) is reset AT ONCE -- no easing out -- so the renderer frees the weather module
 *           (grids, tracks, avatar snow, maps) the next recording and nothing is held while off.
 *   LOCAL   the user's own choice (Weather floater: WeatherEnabled, WeatherType,
 *           WeatherIntensity, WeatherWind*, WeatherStorm). Only this viewer sees it.
 *   REGION  a parcel's published weather (WeatherAllowRegion) or weather derived from the
 *           region's EEP sky (WeatherFromEnvironment): slweatherregion.h, through
 *           setRegionTarget(). Only fed when the user opted in -- a region's chosen
 *           environment is never overridden without a local opt-in.
 *   none    off (the default): every amount eases to zero, the surfaces dry, and once
 *           nothing is left visible() is false and the renderer frees everything.
 *
 * PLACES (#895). Accumulated weather (snow cover / depth, wetness, run-off) and the renderer's
 * ground maps (the snow grids, outer ring, footprints / tracks, run-off) are kept or not by HOW
 * the agent got where it is:
 *   CROSSING (walking / flying / riding / a vehicle across a region border) and a PARCEL CHANGE
 *   inside a region: nothing is reset. The renderer re-bases its agent-space maps by the origin
 *   shift (llcleanweathervk.h shiftOrigin), so the snow lies on in place. The destination is
 *   judged once the region source has evaluated the new parcel (noteSourceSettled; at once with
 *   no region source; after kMoveDecideSecs at the latest):
 *     LOCAL weather in use                -> everything carries on (it falls on both sides);
 *     region weather, destination snowy   -> the lying snow and wetness stay, the precipitation
 *                                            blends to the new target over its transition;
 *     destination without snow / weather  -> what the OLD place's region weather left (and only
 *                                            that: mRegionAccum) FADES over
 *                                            WeatherCrossingFadeSeconds (0 = the natural melt /
 *                                            dry rates); wetness also fades unless it rains there.
 *     The region source holds its old feed through a crossing until the new region's agent
 *     parcel is known (bounded), so a snowy-to-snowy walk has no dip in the snowfall.
 *   TELEPORT (to another region, or inside the region farther than WeatherTeleportKeepMeters):
 *   a NEW PLACE. placeSerial() increments: the renderer drops its ground maps (resetGround: the
 *   grids refill at the global depth, no footprint pattern carries over) without freeing its
 *   module (it frees as usual once nothing is visible and no avatar is wet; the per-avatar
 *   wetness outlives it on the CPU side and dries on). With LOCAL weather the
 *   global amounts carry on too (it falls there as well). Otherwise every amount, the strikes
 *   and the region target are dropped (resetSerial also increments: the audio cuts its beds and
 *   thunder) and, with WeatherTeleportSeed, the destination is SEEDED at its steady state once
 *   its source is known: snowing there -> full cover and WeatherSnowMaxDepth x intensity,
 *   raining -> wet -- arriving in a snowy sim shows a settled blanket instead of minutes of bare
 *   ground. A shorter in-region hop keeps everything (the camera barely moved). A login is the
 *   first region: nothing has fallen yet.
 *   MASTER OFF (#904): the clean slate -- every amount at once, clearSerial() (the renderer frees
 *   its whole module) and resetSerial() (audio, region source).
 * The region source never applies a parcel the agent is not on (the previous region's agent
 * parcel lingers until the new one's properties arrive): slweatherregion.cpp.
 *
 * TRANSITIONS: every amount eases exponentially toward the resolved target with a time
 * constant of a third of the target's transition time (~95 % there after it); the wind is
 * eased as a vector (no 359 -> 0 degree spin). Wetness is integrated, not eased: it soaks
 * up while it rains (WeatherWetSeconds at full rain) and dries once the rain stops
 * (WeatherDrySeconds), so puddles outlast the shower.
 *
 * SNOW COVER (M1.5; WeatherSnowCoating) is integrated the same way: it builds while it snows
 * (WeatherSnowBuildSeconds at full snow) and melts once the snow stops (WeatherSnowMeltSeconds;
 * rain melts it faster). Melting snow turns to water: what melts is added to the wetness, and
 * the surfaces do not start drying until the cover is gone. Snow falling never wets.
 *
 * PRECIPITATION RATE (#954): every source asks for a PHYSICAL rate per type -- rain in mm/h
 * (Target::rainFall; light < 2.5, moderate 2.5-7.6, heavy 7.6-50, violent 50+), snow in cm/h of
 * snowfall (Target::snowFall; light < 0.5, moderate 0.5-2.5, heavy 2.5-8, blizzard-grade 8+ with
 * wind). The 0..1 amounts (Target / State::rain, snow) every older consumer reads are DERIVED from
 * it on one fixed, roughly logarithmic ladder (amountFromFall / fallFromAmount): rain 0.25 ~1 mm/h,
 * 0.5 ~5, 0.75 ~23, 1 = 100 mm/h; snow 0.25 ~0.4 cm/h, 0.5 ~1.5, 0.75 ~5, 1 = 15 cm/h -- so the old
 * WeatherIntensity keeps meaning what it looked like in the middle and its top is now a downpour /
 * a blizzard. Rates above the ladder's top (to kRainFallMax / kSnowFallMax) still intensify the look
 * (the renderer reads the rate) with the amount held at 1.
 *   BANDS: the rate varies over time -- the TARGET's rate x fallVariationAt(seed, variation, synced
 *   UTC): a ~4 minute band and a ~45 s cell (multiplicative: exp(1.1 x variation x noise), 0.33 ..
 *   3 x at variation 1) and, in some 20-minute windows, a DOWNPOUR / SQUALL passing (a bump of
 *   1.6 .. 2.6 x over a minute or two). A pure function of (source seed, synced UTC): every viewer on
 *   the parcel sees the same storm. The bands only shape the precipitation (amounts, rates, the
 *   visibility and the local build-up); everything a place is judged by (snowing there or not, the
 *   shared level, the strikes) reads the unvaried target. The state eases toward the varied target
 *   in amount space (a perceptually even ramp); State::rainFall / snowFall is that eased rate.
 *   VISIBILITY: State::precipExt (1/m) is the precipitation's own extinction (rain ~3e-4 x R^0.75,
 *   snow ~2.5e-3 x S^0.8: ~3 km in light rain, ~300 m in a downpour, ~130 m at 15 cm/h) plus BLOWING
 *   SNOW (State::blowing 0..1: wind 9 .. 18 m/s over lying snow lifts it -- near whiteout with the
 *   snowfall), x WeatherFogAmount. The renderer injects it into the volumetric fog's froxels where it
 *   falls (open sky only, moving with the wind; the blowing part hugs the ground), or -- fog off --
 *   draws a cover-aware analytic veil. precipFog is 0 since then: one physical source; a tag's
 *   explicit fog= stays an extra fog.
 *
 * SNOW DEPTH (M3; #954 physical): State::snowDepth integrates beside the cover -- the SNOWFALL RATE
 * (cm/h) x (1 - kSnowSettle: fresh snow settles by about a third as it lies) while it snows -- 5 cm/h
 * for 10 minutes lays ~0.58 cm --, melting at WeatherSnowMaxDepth per WeatherSnowMeltSeconds once
 * the snow stops (faster in rain), capped at WeatherSnowMaxDepth x snowCover. (WeatherSnowDepthPerHour
 * is no longer read.) It is the GLOBAL accumulated depth: the renderer's persistent depth field
 * (llcleanweathervk.h) fills any texel it enters to it at once.
 *
 * WETNESS (#954): soaks at WeatherWetSeconds for moderate rain (kWetRefFall, 5 mm/h: the old soak at
 * the default intensity), proportionally faster in heavier rain (a downpour in seconds) and slower
 * in a drizzle (clamped 0.05 .. 6 x).
 *
 * SHARED SNOW DEPTH (#783; a parcel tag's depth= / rate= / since=): with the REGION source and a
 * tag that carries depth=, snowDepth is not integrated but COMPUTED every frame -- snowLevelAt():
 * clamp(depth + rate x (now - since), 0, cap) on synced UTC -- so everyone on the parcel sees the
 * same level whenever they arrived (the cap is the user's WeatherSnowMaxDepth). An ARRIVAL (login,
 * a teleport, a reset) sets it at once (the renderer's fresh grids fill at the level); a tag change
 * (an owner edit) or a move onto a parcel with another tag re-syncs over
 * WeatherSnowSyncFadeSeconds (the renderer's grids follow the global level's change both ways).
 * While the level is above zero the cover stays on (a held depth does not melt with the natural
 * cover); at zero the cover melts naturally. snowDepthBuild / snowDepthMelt publish the rates
 * (the tag's rate, else the snowfall rate x (1 - kSnowSettle)) for the renderer's grids and the
 * avatars' snow. Drifts, slump / repose, cover rules, footprints, tracks and compaction stay local:
 * the renderer shapes the shared level. A tag with rate= only (no depth=): only the build rate.
 * #954: a tag with depth= / since= and snowcm= but NO rate= grows by the tag's MEAN snowfall x
 * (1 - kSnowSettle) while it snows (the bands average out; still a pure function of the tag). rate=
 * stays the explicit override; the floater writes it (from the snowfall by default), so viewers
 * older than #954 agree on the level.
 *
 * WIND (M2): the eased target is the slow MEAN (State::windMean -- the renderer's tilted roof
 * map follows it); State::wind adds the gusts: the mean scaled by 1 + variation x noise (so
 * WeatherWindVariation 0.5 swings 50..150 %) and turned by a slight wander (up to ~18 degrees x
 * variation). WeatherWindDirVariation (windDirVariation, degrees 0..180) adds a separate swing
 * of the direction on top: +- that many degrees around the set direction (180: a fully variable
 * wind), smooth, seeded and clock-driven like the speed (own salts; the gustiness mixes in
 * faster veers). At 0 (the default, and any old tag) nothing is added: the slight wander above
 * is the whole behaviour. The noise is three fixed octaves (a ~45 s swell, a ~12 s surge, ~1.6 s gusts)
 * mixed by the gustiness (0 slow swells .. 1 sharp gusts); fixed frequencies, so moving a
 * slider never jumps the phase. It is a pure function of (source seed, synced UTC seconds):
 * every viewer with the same source sees the same gusts at the same moment.
 *
 * LIGHTNING (M2): strikes are a Poisson process in 1 s slots of synced UTC time -- a slot
 * strikes iff hash(seed, slot) < rate x 1 s, rate = kMaxStrikeRate x storm x (0.2 + 0.8 x
 * precipitation) from the resolved TARGET (not the eased state: viewers agree). The same hash
 * gives the moment inside the slot, the distance (0.3..10 km, nearer in a heavier storm), the
 * azimuth (around the upwind side, any side when close), the elevation and the flash shape
 * (2..4 return strokes over 0.2..0.6 s). Strikes are known kLookahead early (the audio decodes
 * the thunder before the flash); State::lightning / lightningDir / lightningDist is the
 * brightest flash this frame. While the storm eases in, a deterministic share of the strikes
 * is held back (a storm builds up instead of starting at full rate).
 *
 * RUN-OFF (M4): State::runoff 0..1 is how strongly water streams off covered surfaces (the
 * renderer's drips, streams, drip-line wetting and rivulets, llcleanweathervk.h M4). It follows
 * the rain once the surfaces are wet (none on the first dry drops: the roofs soak first) plus
 * the melt water of lying snow, rising over a few seconds; when the rain stops it DRAINS over
 * WeatherRunoffDrainSeconds (a third of it is the time constant), never above the wetness, so the
 * last drips taper away with the drying surfaces. runoffNear (audio): how loud the run-off is
 * near the listener, from the renderer's GPU readback (0 when it has none).
 *
 * LISTENER COVER (M1.5, weather audio): the renderer reads the roof map at the audio listener
 * (a GPU readback, a few frames late) and reports it with setListenerOpen(); the audio side
 * (slweatheraudio.h) reports where the listener is relative to the camera with
 * setEarOffset(). Both are plain values -- neither side touches the other.
 */

#ifndef SL_WEATHER_H
#define SL_WEATHER_H

#include "stdtypes.h"

#include <atomic>
#include <vector>

class SLWeather
{
public:
    enum EPrecip : U32 { PRECIP_RAIN = 0, PRECIP_SNOW = 1 };
    enum ESource : U32 { SOURCE_NONE = 0, SOURCE_LOCAL = 1, SOURCE_REGION = 2 };

    // One source's request: what the weather should become.
    struct Target
    {
        bool active = false;      // false: this source asks for nothing
        F32  rain = 0.f;          // precipitation amounts 0..1 per type
        F32  snow = 0.f;
        F32  windDirDeg = 0.f;    // meteorological: where the wind blows FROM, degrees clockwise from north (+Y)
        F32  windSpeed = 0.f;     // m/s
        F32  fog = 0.f;           // extra fog 0..1 (drives the volumetric fog, #718)
        F32  cloud = 0.f;         // cloud cover 0..1 (later: drives the cloud layer, #719)
        F32  transitionSecs = 20.f;
        // M2:
        F32  windVariation = 0.f; // 0..1: the gusty speed swings (1 +- this) x the mean
        F32  windGustiness = 0.5f;// 0 slow swells (tens of seconds) .. 1 sharp gusts (1-2 s)
        F32  windDirVariation = 0.f; // degrees 0..180: extra swing of the direction around windDirDeg (0: only the slight wander)
        F32  storm = 0.f;         // 0..1 lightning amount (0 = none)
        U32  seed = 0;            // the source's seed: gusts and strikes are a function of (seed, UTC)
        // SHARED SNOW DEPTH (#783; parcel tag depth= / rate= / since=, doc/weather_region_protocol.md).
        // haveDepth: the snow level is a pure function of the tag + synced UTC (snowLevelAt()),
        // the same on every viewer on the parcel, instead of what has built up locally since this
        // viewer arrived. haveRate without haveDepth: only the per-second build while it snows
        // follows snowRate (the level itself stays local). Neither: the local build-up (today's).
        bool haveDepth = false;
        bool haveRate = false;
        bool haveSince = false;
        F32  snowDepthAt = 0.f;   // m: the parcel's open-sky snow depth at snowSince
        F32  snowRate = 0.f;      // m/h: + accumulates while it snows (snow > 0), - melts, 0 holds
        F64  snowSince = 0.0;     // unix seconds (synced UTC) the depth applied at (haveSince false: "now")
        // #954 PRECIPITATION RATE (slweather.h): the physical rates (the source of truth; rain / snow
        // above are derived from them -- setFall keeps the pair consistent) and how strongly they
        // vary over time (0 flat .. 1 strong bands / downpours passing). haveSnowFall: a tag named
        // snowcm= (a depth= / since= without rate= then grows by it).
        F32  rainFall = 0.f;      // mm/h
        F32  snowFall = 0.f;      // cm/h of snowfall (fresh, before settling)
        F32  fallVariation = 0.f;
        bool haveSnowFall = false;
        // Sets a type's rate (clamped to its range) and its derived 0..1 amount together.
        void setFall(EPrecip type, F32 fall);
    };

    // One lightning strike (M2). Times are this process's frame clock
    // (LLFrameTimer::getElapsedSeconds); syncT0 is the synced UTC second it was scheduled for.
    struct Strike
    {
        U32 serial = 0;           // increasing, per process (the audio takes each once)
        F64 t0 = 0.0;             // the flash starts (may lie up to kLookahead ahead)
        F64 syncT0 = 0.0;
        F32 dist = 0.f;           // m
        F32 dir[3] = {0.f, 0.f, 1.f};   // unit, camera -> strike, region axes, z up
        F32 duration = 0.3f;      // s: the flicker
        U32 hash = 0;             // per-strike bits (thunder variant / pitch)
    };
    static constexpr F64 kLookahead = 1.5;

    // The live, smoothed state the renderer draws.
    struct State
    {
        F32 rain = 0.f;
        F32 snow = 0.f;
        // #954: the physical rates falling now (the bands included, eased): rain mm/h, snow cm/h.
        F32 rainFall = 0.f;
        F32 snowFall = 0.f;
        // #954 VISIBILITY: the precipitation's own extinction (1/m; blowing snow included, x
        // WeatherFogAmount) and the blowing snow 0..1 (strong wind over lying snow).
        F32 precipExt = 0.f;
        F32 precipExtBlow = 0.f;    // (the blowing snow's part of precipExt: it hugs the ground)
        F32 blowing = 0.f;
        F32 wind[2] = {0.f, 0.f};   // velocity (m/s), region axes: x east, y north (gusts included)
        F32 windMean[2] = {0.f, 0.f};   // M2: the slow mean wind, no gusts (the tilted roof map follows this)
        F32 fog = 0.f;
        F32 cloud = 0.f;
        F32 wetness = 0.f;          // surface wetness 0..1
        F32 snowCover = 0.f;        // snow lying on open, upward surfaces 0..1
        // M3: the open-sky snow DEPTH (m): what an open, level surface holds now. Builds with
        // the snowfall (#954: its rate x (1 - kSnowSettle)), melts after it, never deeper than
        // WeatherSnowMaxDepth x snowCover (so it melts away with the cover). The renderer's
        // depth field starts every texel it has not seen at this depth; only local edits
        // (drifts, footprints) are its own.
        F32 snowDepth = 0.f;
        // #783 SHARED SNOW DEPTH: the rates the depth builds / melts at right now (m/s, >= 0), from
        // the snowfall rate (#954) -- or a parcel tag's rate -- so the renderer's grids, the
        // avatars' and the instances' snow follow the same numbers as snowDepth (no re-derivation
        // from the settings there). A tag-held depth (rate 0) publishes 0 / 0: nothing melts.
        F32 snowDepthBuild = 0.f;
        F32 snowDepthMelt = 0.f;
        // The level is tag-driven (snowLevelAt): the parcel's shared depth, not a local build-up.
        bool snowDepthShared = false;
        // M4 RUN-OFF 0..1: water streaming off covered surfaces (follows the rain on wet surfaces
        // and melt water; drains after; never above the wetness while wet surfaces are on).
        F32 runoff = 0.f;
        // M2 LIGHTNING: the current flash (0 = none .. 1 = peak), set by the state side, drawn by the
        // renderer (sky + scene light pulse). lightningDir: unit vector from the camera towards the
        // strike (region axes, z up); lightningDist: metres to the strike.
        F32 lightning = 0.f;
        F32 lightningDir[3] = {0.f, 0.f, 1.f};
        F32 lightningDist = 0.f;
        ESource source = SOURCE_NONE;
        // Anything left to draw (precipitation, wet or snowy surfaces): false = the renderer's
        // weather module may not exist at all.
        bool visible() const
        {
            return rain > kEpsilon || snow > kEpsilon || wetness > kEpsilon || snowCover > kEpsilon || lightning > kEpsilon
                || runoff > kEpsilon;
        }
    };
    static constexpr F32 kEpsilon = 1e-3f;

    static SLWeather& instance();

    // MASTER SWITCH (WeatherMasterEnabled, default on): off = no weather from ANY source (local,
    // parcel, sky). The settings keep their values; the weather simply resolves to nothing, eases
    // out like any stop, and once visible() is false the renderer frees everything. THE one gate:
    // resolve() (the source layering), the region source's update and the strike list read it.
    static bool masterEnabled();
    // The deepest snow (m) the depth setting / simulation accepts (WeatherSnowMaxDepth's top).
    static constexpr F32 kSnowDepthMax = 10.f;

    // Once per frame (the renderer calls it from its target-ensure step; repeated calls in
    // one frame are no-ops): resolves the sources and steps the transitions.
    void update();
    const State& state() const { return mState; }
    // The target resolved by the last update (which source, what it asks for).
    const Target& target() const { return mTarget; }
    // The user's own weather settings as a target (inactive while WeatherEnabled is off; NOT
    // gated by the master switch -- resolve() does that).
    Target localSettings() const { return localTarget(); }
    // Recent and upcoming strikes (at most a few; the oldest are dropped ~40 s after their flash).
    const std::vector<Strike>& strikes() const { return mStrikes; }
    // UTC seconds (sub-second, corrected by the server's clock offset): the shared time base
    // of the gusts and the strikes.
    static F64 syncedSeconds();
    // The extra fog a source without fog= asks for: 0 since #954 (the precipitation's own medium is
    // State::precipExt). Kept for the tag's fog= default / round trip.
    static F32 precipFog(F32 rain, F32 snow);
    // #954 PRECIPITATION RATE LADDER (slweather.h): the 0..1 amount of a physical rate (rain mm/h,
    // snow cm/h; passes 1 above the ladder's top -- callers clamp) and back. Pure, monotonic.
    static F32 amountFromFall(EPrecip type, F32 fall);
    static F32 fallFromAmount(EPrecip type, F32 amount);
    // The ladder's top (amount 1) and the largest rate accepted (settings, tags, the floater).
    static constexpr F32 kRainFallTop = 100.f, kRainFallMax = 200.f;   // mm/h
    // Snow goes far past nature (360 cm/h = 1 mm/s of fresh snow) so a place can bury itself fast;
    // everything that LOOKS at the rate (visibility, flakes, curtains) saturates at kSnowFallLookMax,
    // a heavy blizzard -- only the build-up keeps following the rate above it.
    static constexpr F32 kSnowFallTop = 15.f, kSnowFallMax = 360.f;    // cm/h
    static constexpr F32 kSnowFallLookMax = 30.f;                      // cm/h
    // A rate's class: 0 light (drizzle / flurries), 1 moderate, 2 heavy, 3 violent / blizzard-grade
    // (the floater's preset ladder and status).
    static U32 fallClass(EPrecip type, F32 fall);
    // BANDS (slweather.h): the rate multiplier at the synced UTC second (1 at variation 0). Pure.
    static F32 fallVariationAt(U32 seed, F32 variation, F64 syncNow);
    // Fresh snow settles by this share as it lies (the build-up per cm of snowfall; a protocol
    // constant -- the shared level uses it too, so it is not a setting).
    static constexpr F32 kSnowSettle = 0.3f;
    // The precipitation's own extinction (1/m, before WeatherFogAmount) for rain mm/h, snow cm/h
    // and the blowing snow 0..1 (slweather.h VISIBILITY).
    static F32 precipExtinction(F32 rainFall, F32 snowFall, F32 blowing);
    // #783 SHARED SNOW DEPTH: a tag's open-sky snow level (m) at the synced UTC second `syncNow`:
    //     clamp(depth + rate x max(now - since, 0) - heat melt, 0, cap)
    // with the rate's growth only while the tag snows (snow > 0; a melting rate always applies)
    // and cap the user's WeatherSnowMaxDepth (0 with WeatherSnowCoating off). A pure function of
    // the tag and the time: every viewer on the parcel computes the same level. -1: t has no depth.
    static F32 snowLevelAt(const Target& t, F64 syncNow);
    // #949 HOOK (not implemented): the melt (m) deterministic heat sources (fires, ...) add to a
    // tag's level between `since` and `syncNow`. Whatever goes here must be a pure function of
    // (tag, region / parcel content every viewer sees, synced UTC) -- a replay, never an
    // integration of this viewer's frames -- or the viewers drift apart. 0 for now.
    static F32 snowHeatMelt(const Target& t, F64 since, F64 syncNow);

    // LISTENER COVER (weather audio). The renderer: the roof map's openness at the listener
    // (0 under cover .. 1 open sky); valid = false when it has no answer (no map, no
    // precipitation simulation running). Read with listenerOpen() (-1: no answer).
    void setListenerOpen(F32 open, bool valid) { mListenerOpen = valid ? open : -1.f; }
    F32  listenerOpen() const { return mListenerOpen; }
    // The audio side: the listener's position minus the camera's (agent axes, m); the
    // renderer samples the roof map there.
    void setEarOffset(F32 x, F32 y, F32 z) { mEarOffset[0] = x; mEarOffset[1] = y; mEarOffset[2] = z; }
    const F32* earOffset() const { return mEarOffset; }
    // M4: the run-off's loudness near the listener 0..1 (the renderer's readback; 0 = none).
    void setRunoffNear(F32 v) { mRunoffNear = v; }
    F32  runoffNear() const { return mRunoffNear; }

    // REGION SOURCE SEAM (#783, later milestones). Only call when the user opted in.
    void setRegionTarget(const Target& t) { mRegion = t; mHaveRegion = true; }
    void clearRegionTarget() { mHaveRegion = false; }

    // CLEAN SLATE (#904; slweather.h PLACES). Every accumulated amount, the pending strikes, the
    // region source's target and the listener / run-off readbacks go at once. Applied by the next
    // update() (the main thread); clearSerial() and resetSerial() increment then. `why` is logged.
    void requestReset(const char* why);
    // Incremented by every clean slate (master off) and every teleport that drops the old place's
    // weather (not local): the weather audio cuts its beds / thunder, the region source drops its
    // tag cache and re-polls.
    U32  resetSerial() const { return mResetSerial.load(std::memory_order_acquire); }
    // Incremented by the clean slate only: the renderer frees its whole weather module.
    U32  clearSerial() const { return mClearSerial.load(std::memory_order_acquire); }
    // Incremented by every NEW PLACE (a teleport, slweather.h PLACES): the renderer drops its
    // ground maps (snow grids, ring, tracks, run-off), keeping the module and the avatars' snow /
    // wetness; the region source re-polls.
    U32  placeSerial() const { return mPlaceSerial.load(std::memory_order_acquire); }
    // The region source has evaluated the agent's parcel `parcelId` (-1: no parcel source) in the
    // region `regionHandle` (fed or not): a change of either from the last one is a crossing /
    // parcel change whose destination can now be judged (slweather.h PLACES).
    void noteSourceSettled(U64 regionHandle, S32 parcelId);
    // Watches the agent's region (idempotent; the region source's idle calls it): a region change
    // inside a teleport marks the teleport as one to another region; one without is a crossing
    // (slweather.h PLACES).
    static void watchRegion();

private:
    SLWeather() = default;
    Target localTarget() const;
    void   applyReset(const char* why);
    void   onRegionChanged();
    // PLACES (#895): the teleport watch, a new place (teleport) and a crossing / parcel change's
    // decision once the destination is known (update()).
    void   watchTeleport();
    void   newPlace(const char* why, F64 dist);
    void   decideMove(const Target& t, ESource source);
    void   seedPlace(const Target& t, ESource source);
    void   stepFade(const Target& t, F32 dt);
    // #783 SHARED SNOW DEPTH: a tag-driven level (snowLevelAt) applied / re-synced (update()).
    void   stepDepthSync(const Target& t, ESource source, F32 level, F32 dt, F64 syncNow);
    Target resolve(ESource& source) const;
    void   stepWind(const Target& t, F64 syncNow);
    void   stepLightning(const Target& t, F32 dt, F64 syncNow, F64 now);

    State  mState;
    Target mTarget;
    std::vector<Strike> mStrikes;
    S64    mNextSlot = 0;
    bool   mSlotValid = false;
    U32    mStrikeSerial = 0;
    F32    mStormEase = 0.f;
    Target mRegion;
    bool   mHaveRegion = false;
    double mLastTime = -1.0;
    U32    mLastFrame = ~0u;
    F32    mListenerOpen = -1.f;
    F32    mEarOffset[3] = {0.f, 0.f, 0.f};
    F32    mRunoffNear = 0.f;
    // RESET (#904): requested by the master switch, applied in update().
    std::atomic<const char*> mPendingReset{nullptr};
    std::atomic<U32> mResetSerial{0};
    std::atomic<U32> mClearSerial{0};
    std::atomic<U32> mPlaceSerial{0};
    bool   mMasterOn = true;        // (the master switch's last state: the off edge resets once)
    bool   mRegionAccum = false;    // something lying now came from a REGION source (a move away fades it)
    U64    mRegionHandle = 0;       // the agent's region at the last region-changed signal (0: none yet)
    // PLACES (#895; slweather.h). The teleport in progress: where it started (global), whether
    // the region changed inside it, whether it was already taken as a new place.
    bool   mTpActive = false, mTpRegionChanged = false, mTpDone = false;
    F64    mTpFrom[3] = {0.0, 0.0, 0.0};
    // The last place the region source settled on (0 / -1: none since login / the last teleport).
    U64    mSettledHandle = 0;
    S32    mSettledParcel = -1;
    // A crossing / parcel change waiting for its destination: why, and the latest decision time
    // (frame clock); mMoveSettled: the region source has evaluated the destination.
    const char* mMoveWhy = nullptr;
    F64    mMoveDeadline = 0.0;
    bool   mMoveSettled = false;
    // A teleport's seed waiting for the destination's source (until mSeedDeadline).
    bool   mSeedPending = false, mSeedSettled = false;
    F64    mSeedDeadline = 0.0;
    // The fade after a move to a place without snow (seconds left; 0 = none) and what it fades.
    F32    mFadeLeft = 0.f;
    bool   mFadeSnow = false, mFadeWet = false;
    // #783 SHARED SNOW DEPTH: the tag (depth, rate, since, snowing) last applied, the re-sync fade
    // (from what depth / cover, seconds left of how many) and whether the next application is an
    // ARRIVAL (login, a teleport, a reset: set at once -- the grids start fresh at the level).
    bool   mSyncActive = false;
    bool   mSyncArrival = true;
    F32    mSyncDepth = 0.f, mSyncRate = 0.f;
    F64    mSyncSince = 0.0;
    bool   mSyncSnowing = false, mSyncHaveSince = false;
    F32    mSyncFromDepth = 0.f, mSyncFromCover = 0.f;
    F32    mSyncFadeLeft = 0.f, mSyncFadeTotal = 0.f;
};

#endif
