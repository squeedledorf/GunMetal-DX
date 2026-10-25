/**
 * @file slweatheraudio.h
 * @brief Weather ambience (#783): recorded rain, roof, drip, wind, snow (and hail) beds that
 *        follow the weather state, outdoor beds giving way to indoor ones under cover.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slweather.h)
 * $/LicenseInfo$
 *
 * SOUNDS: the weather sound pack (shipped in the repo under app_settings, CC0 Freesound recordings; see
 * app_settings/sounds/weather/SOURCES.md), app_settings/sounds/weather/<name>.ogg: 48 kHz,
 * 1 or 2 channels, every *_loop.ogg loops over the whole file (the crossfade is baked in).
 * Each bed is loudness-normalised to its own target (manifest.json: a heavy-rain bed is
 * louder than a light one by design), so the mix only applies the manifest's default_gain
 * and the layer weights below. No pack: logged once, silent (there is no fallback sound).
 *
 * LAYERS (equal power inside each group; m = cover 0..1, open = sqrt(1-m), cov = sqrt(m)):
 *   rain      rain_light / medium / heavy (peaks at rain 0.25 / 0.6 / 1)        x open
 *   roof      roof_wood(_heavy), roof_metal(_heavy), roof_glass, roof_tent      x cov
 *             by the room acoustics' overhead material shares (LLCleanAcoustics::
 *             listenerRoofMaterial: wood unless it measured otherwise); the heavy variants
 *             cross in above rain 0.45
 *   drip      drip_loop while it rains or the surfaces are still wet (draining:
 *             only with a live cover answer, i.e. the room acoustics)            x cov
 *             M4: at least the run-off's level near the listener (eaves dripping, streams
 *             pouring within ~12 m: SLWeather::runoffNear, a GPU readback), in the open too
 *   hail      hail_outdoor x open, hail_roof x cov (prepared: there is no hail state yet)
 *   wind      wind_light / gust / howl by wind speed (4 / 9 / 15 m/s), only with
 *             precipitation, about a third in snow                              x open
 *             wind_indoor (snow: snow_wind_muffled) by wind speed                 x cov
 *             #894 through the openings: wind_whistle(_b), wind_howl_window, the
 *             mono rush (wind_light / wind_gust), placed at the openings          x cov
 *   snow      snow_ambience x open, snow_interior x cov
 * All at AudioLevelAmbient x WeatherAudioVolume through the engine's local loops
 * (LLAudioEngine::createLocalLoop: listener-relative, no asset, no in-world voice taken).
 * Layer gains ease in over ~0.6 s and out over ~1.8 s on top of the input smoothing, so
 * stepping under a roof never switches audibly. A bed that is still loading lends its
 * share to its loaded neighbour (no dip while the next intensity's bed decodes).
 *
 * COVER: from the room acoustics when it is on and has measured the room
 * (LLCleanAcoustics::listenerRoom: the ray-traced roof fraction, deepened by the enclosure --
 * a porch counts less than a closed room), else from the weather roof map at the listener
 * (SLWeather::listenerOpen: a GPU readback of the coarse map, a few frames late). Camera under
 * water: the covered beds, quieter.
 *
 * GUSTS: the wind beds follow the gusty wind (SLWeather::State::wind; speed eased over 0.3 s
 * only, so a 1-2 s gust is heard).
 *
 * WIND THROUGH OPENINGS (#894, WeatherAudioWindOpenings): under cover the wind is shaped by the
 * openings around the listener -- LLCleanAcoustics::listenerOpenings: the room rays' escaped /
 * far directions grouped into up to 4 openings (direction, solid angle, distance), from the
 * acoustics' own rays or, with AudioAcoustics off, room-only requests the weather asks for while
 * it is windy under cover (wantOpenings; ray tracing needed). Each opening is weighted by how
 * much it faces into the wind (its outward horizontal direction against where the wind comes
 * from: 1 windward, 0.2 lee, 0.5 straight up) and split by size into crack (< ~0.05 sr: the
 * high wind_whistle), small (~0.1-0.25 sr: wind_whistle_b), window-sized (~0.3-0.7 sr:
 * wind_howl_window, strong wind only) and wide (> ~0.75 sr: a broadband rush, wind_light /
 * wind_gust decoded to mono). Level from the windward solid angle of each class, x the cover
 * (sqrt m), quieter in snow; the sealed-room bed (wind_indoor / snow_wind_muffled) gives way by
 * up to half as the openings grow -- a sealed room sounds as before. The whistles and the rush
 * are PLACED (LLAudioEngine::setLocalLoopOffset) toward the dominant opening of their class, at
 * the wall it sits in (1.5-8 m, no distance attenuation), the direction eased over 0.5 s.
 * Gusts (the wind against its mean) lift the openings' level (x0.4-1.6) and the whistle's pitch
 * (+-15 %). No rays: the roof map's openness at the listener as one wide, unplaced opening.
 * Cost: nothing unless it is windy with precipitation under cover; then 64 room rays every
 * AudioAcousticsRoomInterval frames (shared with the acoustics when it runs) and a 64-bin pass
 * per room answer. A ~10 s log line lists the openings and the levels.
 *
 * LOOP VARIATION (#1012, WeatherAudioLoopVariation; off = each bed one plain loop): the recordings are
 * 43-67 s, so the sequence repeats audibly. Every bed has TWO PLAYHEADS over its one decoded buffer
 * (LLAudioEngine::createLocalLoopShared: a second source, no copy); every 10-25 s (random per bed)
 * the idle one is moved to a fresh start point (seekLocalLoop) and the two swap with a 2-4 s
 * equal-power (cos / sin) cross-fade. The start point is a random window of the bed's coarse RMS
 * envelope (240 ms windows, computed with the decode, off the main thread) at or above the median --
 * never a quiet stretch -- and at least 4 s from the playing head. Both heads take the bed's gain,
 * group cross-fade, duck, pause and #894 placement. SPARSE beds (LayerDef::sparse: rain_light, drip)
 * also get random one-shot drops: up to 12 slices (80-250 ms, 4 ms in / smooth out, mono) cut at the
 * loudest transients of the bed's own 20 ms RMS (local maxima above the 90th percentile and 3x the
 * median, 0.6 s apart), played at Poisson-ish times (0.7/s at full level, scaled by the bed's level)
 * as placed engine shots 1.5-6 m around the listener at 0.3 x the bed's level. And each bed's level
 * drifts +-1 dB (a random target every 10-30 s, twice low-passed over 3 s). The engine has no per-loop
 * filter, so no tone drift. Each bed logs its envelope statistics (debug) when it loads.
 *
 * THUNDER (M2): every lightning strike (SLWeather::strikes, listed ~1.5 s before its flash)
 * becomes one voice: thunder_close (< 0.8 km) / thunder_mid (0.8-3 km) / thunder_distant
 * (> 3 km), a variant of the class not heard in its last 2 strikes, +-5 % pitch. It starts at
 * flash + distance / 343 m/s (beyond 6 km the rest of the delay x WeatherThunderDelayScale;
 * never under 1 s), as an engine local SHOT placed at the strike's direction, at most 120 m
 * out, rolloff 0 (the level is baked per class). Through the room acoustics' source ray
 * (LLCleanAcoustics::traceExtraSource) at 0.3 x the occlusion strength -- thunder diffracts
 * around a hill; under cover the highs go (HF x0.12 fully sheltered) while the boom stays
 * (x0.6), and indoors it sends to the room reverb by the enclosure. A close strike ducks the
 * rain / roof / drip beds by 3 dB (150 ms in, 1.5 s out).
 *
 * MEMORY: a bed is read and decoded (General work queue, libvorbisfile) only when its layer
 * first wants to be heard, handed to OpenAL (which keeps the only copy: ~190 KB per stereo
 * second, 7-16 MB per bed), and freed after 10 s of silence. Decoded beds are capped at
 * kPcmBudget; over it, silent beds are freed first and a new bed waits. Nothing at all is
 * loaded until there is weather to hear (and WeatherAudio is on). Thunder variants (mono,
 * 1-2.5 MB each) share the budget: decoded when a strike picks them, freed after 60 s unused.
 */

#ifndef SL_WEATHERAUDIO_H
#define SL_WEATHERAUDIO_H

class SLWeatherAudio
{
public:
    // Audio idle (llappviewer, after LLCleanAcoustics::update, before gAudiop->idle()).
    static void update();
    // Before the audio engine is shut down: frees the loops; a decode still running on the
    // work queue finishes into memory it owns and is dropped.
    static void shutdown();
};

#endif
