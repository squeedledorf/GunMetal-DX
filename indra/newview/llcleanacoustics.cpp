/**
 * @file llcleanacoustics.cpp
 * @brief Acoustics prototype (#683 phase 0, #857 source rooms, #858 diffraction). See llcleanacoustics.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llcleanacoustics.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llcleaninstancestore.h"
#include "llframetimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"          // #953: the water surface at a point (its region's water height)
#include "material_codes.h"
#include "slweather.h"        // #930: lying snow (wetness, global depth, snowfall)
#include "vkgfx/llvulkancontext.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace
{
constexpr U32 kMaxSrc = LLVKAcousticsRequest::kMaxSources;
constexpr U32 kMaxDiff = LLVKAcousticsRequest::kMaxDiffSources;
constexpr U32 kMaxQueries = LLVKAcousticsRequest::kMaxQueries;
constexpr U32 kProbePer = 16;          // rays per source-room probe (one probe = 16 RAY queries)
constexpr U32 kNone = LLVKAcousticsRequest::kNone;
constexpr U32 kRoomRays = LLVKAcousticsResult::kRoomRays;
constexpr U32 kMaxZones = LLAudioEngine::kAcousticsMaxZones;
constexpr U32 kRing = 16;              // requests remembered for mapping answers back to sources
F32 sRoomRange = 30.f;                 // room ray length (m): AudioAcousticsRoomMaxDist
constexpr F32 kMaxSourceDist = 200.f;  // farther voices are not traced (inaudible; past the TLAS radius soon)
constexpr F32 kRoomTau = 1.5f;         // s: enclosure / room size smoothing
constexpr F32 kOccAttack = 0.25f;      // s: becoming occluded
constexpr F32 kOccRelease = 0.5f;      // s: becoming clear
constexpr F32 kStaleAfter = 2.f;       // s without answers -> decay toward dry / clear
constexpr F32 kStaleTau = 2.f;
constexpr F64 kLogEvery = 10.0;        // s

// ---- #857 SOURCE ROOMS ----
// Each probe answer gives a source kProbePer rays (rotated every time): its room estimate is
// smoothed per source over kSrcRoomTau of real time (~100+ rays in that window at the default
// budget). COUPLING = the smoothed fraction of the probe rays' end points the listener can
// see: in the listener's own room nearly all (a convex room 1.0, a furnished one ~0.7), in
// the next room through a doorway a few, sealed none. Mapped to a SAME-ROOM weight 0..1.
constexpr F32 kSrcRoomTau = 1.0f;
constexpr F32 kCoupleTau = 0.8f;
constexpr F32 kCoupleLo = 0.10f;
constexpr F32 kCoupleHi = 0.50f;
constexpr F64 kProbeStaleLo = 2.0;     // s since the last probe: the estimate starts to count less ...
constexpr F64 kProbeStaleHi = 4.0;     // ... and not at all (back to the listener-only rule)
// PROBE SCHEDULE: every traced source is probed; the AudioAcousticsSourceRooms most audible
// at least every kProbeTop, every other one at least every kProbeLow (as long as the
// AudioAcousticsSourceRoomRays budget allows; the most overdue -- relative to its interval --
// go first, so a shortfall stretches everyone evenly).
constexpr F64 kProbeTop = 0.2;
constexpr F64 kProbeLow = 0.5;
// Per-source records (occlusion + room + diffraction + zone) live while traced (5 s grace),
// never more than kMaxTracked (the least recently traced goes first).
constexpr U32 kMaxTracked = 96;      // (64 traced at once: 30 channels + 34 extra, plus grace)
// REVERB ZONES: sources in other rooms send (second send) to one of up to kMaxZones extra
// reverbs, clustered by room character with hysteresis; a zone switch fades the send out,
// retargets, fades in (no pop). An idle zone is reclaimed after kZoneIdle.
constexpr F32 kZoneSendTau = 0.25f;
constexpr F32 kZoneFadeIn = 0.15f;
constexpr F32 kZoneFadeOut = 0.08f;
constexpr F32 kZoneRoomTau = 1.0f;
constexpr F32 kZoneJoin = 0.20f;       // feature distance to join an existing zone
constexpr F32 kZoneStay = 0.35f;       // ... to stay in the current one
constexpr F64 kZoneIdle = 2.0;
constexpr F32 kZoneMinSend = 0.003f;   // below: no zone wanted

// ---- #858 DIFFRACTION: a PERSISTENT PATH CACHE per source ----
// Paths listener -> P1 (-> P2) -> source found by any ray (the listener room rays' shadow
// rays, the source-room probe, guided discovery rays, two-bounce joins) are KEPT
// (kCachePaths per source; a new one within kPathMerge of a kept one refreshes it) and
// RE-VALIDATED by plain segment queries -- each leg L -> P1 (-> P2) -> S -- every kValidateTop
// for the kTopOccluded most audible occluded sources, every kValidateLow for the rest. A leg
// that fails drops the path at once (a door closing: one frame + readback, ~50-100 ms); one
// not validated for kPathStale stops counting. So sampling luck only decides how fast a
// doorway is FOUND, never whether it is kept.
// DISCOVERY is importance-sampled: guided rays from the listener toward the kept paths'
// points, toward the listener's FAR points (room rays that went much farther than the room:
// openings) and toward the source; from the source toward its kept / far points and the
// listener. TWO-BOUNCE joins connect a listener far point (seen by L) with a source far point
// (seen by S, not by L): a source deep in the next room, seen from no point the listener sees.
// A path's strength falls with its DETOUR (path / direct distance): exp(-(rho - 1) / kDetourScale)
// (a doorway path 40% longer than the wall-blocked straight line keeps ~0.45), times kBounce
// (a bounce / edge loses energy; a second bounce kBounce2 more), times agreement (one path
// 0.8, two or more 1). Its HF is cut with the turn angles (straight through 0, folding back
// kDiffHf). The strength takes back that share of the occlusion muffling.
constexpr F32 kDetourScale = 0.5f;
constexpr F32 kBounce = 0.85f;
constexpr F32 kBounce2 = 0.6f;
constexpr F32 kDiffHf = 0.7f;
constexpr U32 kCachePaths = 4;
constexpr F32 kPathMerge = 0.4f;       // m
constexpr F64 kPathStale = 0.75;       // s since the last validation: not trusted
constexpr F64 kPathExpire = 1.5;       // s: dropped
constexpr U32 kTopOccluded = 6;
constexpr F64 kValidateTop = 0.05;     // s (~every 3rd frame at 60 fps: a door closing reads in ~0.1 s)
constexpr F64 kValidateLow = 0.25;     // s
constexpr F64 kDiscoverTop = 0.1;      // s
constexpr F64 kDiscoverLow = 0.5;      // s
constexpr U32 kGuided = 4;             // guided rays per end per discovery
constexpr U32 kJoinsPerSource = 4;
constexpr U32 kFarPoints = 8;          // the listener's far points (openings)
constexpr U32 kSrcFar = 4;             // per source: points it sees that the listener does not
constexpr F64 kFarAge = 1.5;           // s
constexpr F32 kQAttack = 0.25f;        // s: a path opening
constexpr F32 kQRelease = 0.05f;       // s: a path closing (validation already took ~a frame or three)
constexpr F32 kIndDirTau = 0.3f;       // s: apparent direction smoothing
constexpr F32 kPanTau = 0.35f;         // s: pan weight / direction smoothing (per frame)

// ---- ROOM ABSORPTION MODEL (mean absorption coefficient alpha of the hit surfaces) ----
// SURFACES: the prim's PHYSICAL material (LLPrimitive::getMaterial, the collision-sound
// code) is the one acoustic fact SL content carries. Every new prim is WOOD, so wood is only
// a weak prior that leans hard -- a bare room must stay lively; the GEOMETRY (clutter) is what
// makes a furnished room dead. The render material can only push towards HARD (metal,
// polished dielectric, legacy env reflection: the probe's hint); visual roughness is not
// acoustic absorption (concrete looks rough, alpha ~0.02) and is ignored otherwise.
// {mid (~500 Hz-1 kHz), high (~4 kHz)}:
struct Absorb { F32 mid, hf; };
constexpr Absorb kAbsStone   = {0.03f, 0.05f};
constexpr Absorb kAbsMetal   = {0.03f, 0.04f};
constexpr Absorb kAbsGlass   = {0.03f, 0.03f};
constexpr Absorb kAbsWood    = {0.07f, 0.09f};   // (the default: weak prior)
constexpr Absorb kAbsFlesh   = {0.40f, 0.55f};
constexpr Absorb kAbsPlastic = {0.08f, 0.10f};
constexpr Absorb kAbsRubber  = {0.30f, 0.40f};
constexpr Absorb kAbsTerrain = {0.25f, 0.40f};   // soil / grass
// WATER (#953): a calm water surface is one of the hardest surfaces there is (air / water
// impedance ratio ~3600: ~99.9 % of the energy reflects, from either side). The room rays and
// the source-room / guided probe rays hit it (clean_audio_rt.comp: hint -2, never a blocker);
// before #953 they passed it and scored the bed as terrain -- or escaped over deep water as
// open area --, so a listener over a lake heard a dead, dry space.
constexpr Absorb kAbsWater   = {0.01f, 0.02f};
inline bool waterHint(F32 hint) { return hint < -1.5f; }   // (the probe's -2; terrain is -1)
// FOLIAGE (alpha-mask hits, alpha-tested on the GPU): leaves scatter and let sound through
// (what passes is lost to the room), so a hedge damps and darkens rather than reflects.
// Applies when the prim keeps the default physical material (wood / light); a material
// the owner set explicitly wins (stone / metal / glass / plastic), and the soft ones (flesh /
// rubber) never make foliage livelier than this.
constexpr Absorb kAbsFoliage = {0.50f, 0.70f};
// Foliage counts only partly as WALL for the enclosure / roof fractions and the room-size
// mean: a hedge tunnel is not a closed room (0.4 left a hedge arch beside a building at
// wet ~0.13; 0.25 takes it near-dry, ~0.05).
constexpr F32 kFoliageWall = 0.25f;
// ALPHA BLOCKS (AudioAcousticsAlphaBlocks, default on): the owner's rule is the alpha you
// would see -- fully transparent passes, anything even slightly visible blocks. The probe
// then also commits visible ALPHA-BLEND hits (a window, a glass wall / roof, a 90%-
// transparent pane) on the room rays, and reports the nearest visible blend / alpha-mask
// surface each source ray crosses. A blend hit is a full wall (enclosure / roof / size /
// clutter weight 1) with GLASS absorption unless the prim's physical material says
// otherwise (stone / metal / plastic / flesh / rubber; wood -- every new prim's default --
// and light mean "not set": glass). An alpha-mask prim whose physical material is set HARD
// (stone / metal / glass / plastic: a grille, a fence, a lattice screen) is a solid surface
// too; every other alpha-mask hit keeps the foliage handling above (there is no other
// foliage flag on prim / mesh content: alpha-mask = leaves unless the material says hard).
// OCCLUSION weights (the blocked value a source ray contributes, 1 = opaque wall): a single
// glass pane muffles noticeably but well short of a wall (strength 0.8: about -2.7 dB
// overall / -4.9 dB HF vs -5 / -11 dB for stone); a hard alpha-mask screen as glass /
// a wall by its material; foliage (alpha-mask without a hard material) muffles a little:
// alpha cut-outs are almost always leaves / soft content, rarely a hard surface.
constexpr F32 kOccGlass   = 0.6f;
constexpr F32 kOccFoliage = 0.3f;
bool sAlphaBlocks = true;              // AudioAcousticsAlphaBlocks, latched by fillRequest
constexpr F32 kAbsHard = 0.03f;                  // where a full hardness hint pulls a surface
// CLUTTER (the probe's per-ray geometric score: rays landing in front of the plane of their
// neighbours = furniture, shelves, log bulges; a bare convex room scores exactly 0). The
// mean score saturates at kClutterFull; a fully cluttered room adds the scattering / soft
// furnishing absorption no surface tag tells us about, mostly at high frequencies.
// (Offline sim of the probe's score, 4x5x2.7 m room: bare 0, sparse furniture ~0.05,
// dense ~0.07, log walls 0.0-0.19 depending on the ear's distance to them; club ~0.03.)
// Source rooms (#857) have no clutter score (16 rays are too sparse for the neighbour test):
// they start from the listener's and keep it.
constexpr F32 kClutterFull = 0.10f;
constexpr F32 kClutterMid  = 0.20f;
constexpr F32 kClutterHf   = 0.50f;
// DECAY (Eyring-style, with OPEN AREA): T = kDecayK * room_size / -ln(1 - a), where a is the
// mean absorption over ALL rays (uniform over the sphere = solid-angle weighted): a ray that
// escapes (sky, or nothing within sRoomRange) is an open window, absorption 1.0 at every
// frequency; a hit contributes its surface (+ clutter, biased) + kUnmodelled, the absorption
// the rays never see (doors, leakage, people, avatar geometry the mask leaves out; blend
// geometry too with AudioAcousticsAlphaBlocks off):
//     a = escape + (1 - escape) * min(alpha_hits + kUnmodelled, 0.95)
// A closed room (escape 0) is exactly the old tuning: 2.8 m furnished wood ~0.4 s, 2.8 m bare
// hard ~0.7-0.8 s, ~11 m bare stone hall ~3.3 s, ~8 m cluttered club ~1.3 s. Open and
// semi-open spaces come out short by themselves -- no separate openness factor on decay.
constexpr F32 kUnmodelled = 0.20f;
constexpr F32 kDecayK     = 0.075f;
// ROOM SIZE only means something when enclosed: the (wall-weighted) mean listener -> hit
// distance over the non-floor hits, pulled toward kSizeOpen as the enclosure drops
// (enclosedWeight), so a few distant buildings / hills seen from open ground do not make a
// 13 m "room". The same weight gates CLUTTER: leaf noise and scattered props in the open are
// not a furnished room.
constexpr F32 kSizeOpen   = 1.5f;

// ---- #683 PHASE 1 bounce model constants (see "MEASURED REVERB" below) ----
constexpr F32 kSpeed = 343.f;                        // m/s
// Air absorption (20 C, 50 % RH, ISO 9613-1 order of magnitude) at the bands' centres
// 250 Hz / 1 kHz / 5 kHz, dB per metre; energy per metre = dB / 4.343.
constexpr F32 kAirDbPerM[3] = {0.0011f, 0.005f, 0.04f};
constexpr F32 kRecvR = 1.0f;                         // m: the receiver sphere (diffuse rain / specular detection)
// Absorption the bounce rays never see (doors, leakage, people / avatars the mask leaves out),
// per reflection on top of the surface; the room model's kUnmodelled (0.20) also stands in for
// the open area and furniture, which the bounces trace for real.
constexpr F32 kBounceUnmodelled = 0.10f;
constexpr F32 kPolishedScatter = 0.05f;              // a full hardness hint pulls scattering down to this
constexpr F32 kBounceTau = 0.75f;                    // s: accumulator forgetting
constexpr F32 kReflCal = 0.45f;                      // early / late energy ratio -> REFLECTIONS_GAIN
constexpr F64 kMeasStaleLo = 1.0;                    // s since the last answer: the measurement fades ...
constexpr F64 kMeasStaleHi = 3.0;                    // ... out (back to the statistical model)
constexpr F64 kBounceSrcEvery = 0.15;                // s: a source's own bounce set at most this often
constexpr U32 kSrcBouncePool = 8;                    // source accumulators kept (LRU)

// ---- #683 EARLY GAIN (AudioAcousticsEarlyGain, default off) ----
// The direct path used to be one line (+ occlusion, + #858's paths) and the bounce energy only
// drove the shared reverb, so reflected energy reaching the listener never raised a source's own
// level: a sound inside a metal horn was no louder in front of the mouth. Now the
// AudioAcousticsEarlyGainSources most audible sources get EARLY-GAIN bounce sets: rays FROM the
// source with the LISTENER as receiver (clean_audio_rt.comp EARLY-GAIN SETS: the same diffuse
// rain + specular near-pass estimator, AudioAcousticsEarlyGainBounces bounces,
// AudioAcousticsEarlyGainRays rays), round robin on the bounce frames (each source at most every
// kEarlyEvery, the most overdue first, in the bounce slots the room-decay sets leave free).
//  * MEASURE: per answer and band, the arrivals up to AudioAcousticsEarlyGainWindowMs after the
//    direct sound's time D / c (the echogram up to D / c + window: a reflected path is never
//    shorter than D), over the FREE-FIELD direct energy in the same units:
//        E_free = rr^2 / (4 D^2) x exp(-air D)   per ray
//    (rr = the set's receiver sphere: the deposits are the flux through its cross-section, and the
//    direct path of an isotropic source puts pi rr^2 / (4 pi D^2) of it there). Summed over
//    answers with kEarlyAccTau forgetting: ratio_b = E_early_b / E_free_b, rays-weighted. rr grows
//    with the distance (earlyRecvRadius): fewer missed specular returns at range; the ratio does
//    not depend on it. The FIRST-ORDER floor / ground reflection is left out in the shader: the
//    plain distance model already stands for "a sound over a floor" (sim: open terrain would
//    otherwise read +1.6 dB, a wooden prim platform +2.4 dB, on every sound).
//  * REVERB SHARE: the shared EAX reverb already plays a room's early reflections (its REFLECTIONS
//    stage, from the listener's own room) for a source in that room, so the direct path only
//    takes the part the reverb does NOT render:
//        r_b = fresh x (1 - AudioAcousticsEarlyGainReverbShare x room x same) x ratio_b
//    room = the listener's room-ness (as reverbFromRoom, x the reverb level up to 0.3), same = the
//    source's same-room weight (#857). Outdoors (the horn seen from in front of it) the reverb
//    renders none of it: all of it goes to the direct path. In a room 0.9 of it is the reverb's
//    already (sim, hard 5 x 4 m room, 3 m apart: ratio ~5 -> +1.8 dB instead of +7.9 dB). So the
//    per-source reverb send is NOT reduced: it never carried this source-specific remainder.
//  * LEVEL (energy over the free-field direct at the same distance; g = today's direct gain with
//    occlusion and #858 diffraction, g0 = the same without the diffraction paths):
//        E = g^2 + max(r_mid - (g^2 - g0^2), 0)          (occluded direct + max(diffraction, early))
//        level = g x min(sqrt(E) / g, 10^(AudioAcousticsEarlyGainMaxDb / 20))
//    The diffraction share (g^2 - g0^2) and the reflected energy are largely the SAME paths (#858
//    finds its paths with rays to hit points too), hence the max rather than a sum. Defined against
//    the FREE-FIELD direct energy, never the occluded one, so a source behind a wall cannot
//    explode: r_mid there is only what really routes round (sim: 0.00-0.01). With r = 0 (open
//    field, or the setting off) the multiplier is exactly 1: today's behaviour.
//  * HF (5 kHz, a = g x today's HF filter, a0 likewise without diffraction): E_hf = a^2 +
//    max(r_high - (a^2 - a0^2), 0); HF filter = today's x sqrt(E_hf / a^2) / sqrt(E / g^2), at most
//    1 overall (the filter can only cut). Equal to today's when r = 0.
//  * WINDOW (AudioAcousticsEarlyGainWindowMs, 50 ms): the energy that FUSES with the direct sound
//    (the precedence / Haas zone, the D50 "early useful energy" bound) is heard as the source being
//    louder; later arrivals are heard as separate reflections / reverb, which the EAX stage renders
//    (its REFLECTIONS / late reverb), so they do not belong in the direct level. A long horn does
//    not need a longer window: its near-axial paths, the ones that funnel, arrive within it (sim,
//    25 m hollow horn: early/direct ~30 at 50 ms, the +9 dB cap is reached at ~7).
//  * SCHEDULING: a sound without a confident measurement yet (accumulated rays < kEarlyRaysHi)
//    takes every bounce frame until it has one, ahead of the round robin; and a SPATIAL CACHE
//    (egCacheFind: by emitter AND listener position, kEgCacheMatch) seeds a sound that starts where
//    something was measured recently -- a gesture one-shot (a new source id every trigger), the next
//    clip of a chain -- which then starts at its level without the attack ramp.
//  * APPLIED: both multipliers smoothed in dB (kEarlyAttack up, kEarlyRelease down; fading out
//    kEarlyStaleLo .. kEarlyStaleHi after the last answer, and with too few rays); the level is
//    split into the direct filter (<= 1) and a source-gain BOOST (>= 1: AL_GAIN with AL_MAX_GAIN
//    raised, LLAudioChannel::setAcousticsBoost). The engine divides the reverb / zone sends by the
//    boost so they keep their level; the #897 bass send follows the dry path, boost included.
constexpr F64 kEarlyEvery = 0.1;          // s: a source's early-gain set at most this often
constexpr F64 kEarlyHold = 0.25;          // s: a dispatched set not yet answered blocks a re-pick this long
constexpr F32 kEarlyAccTau = 0.5f;        // s: accumulator forgetting
constexpr F32 kEarlyAttack = 0.15f;       // s: the applied level rising
constexpr F32 kEarlyRelease = 0.3f;       // s: ... falling
constexpr F64 kEarlyStaleLo = 0.6;        // s since the last answer: the measurement fades ...
constexpr F64 kEarlyStaleHi = 1.5;        // ... out (back to today's level)
constexpr F32 kEarlyRaysLo = 64.f;        // accumulated rays: confidence ramp
constexpr F32 kEarlyRaysHi = 384.f;
constexpr F32 kEarlyMinDist = 0.75f;      // m: closer, the direct sound dominates (not traced)
// Material classes of the bounce pass (clean_audio_rt.comp ACC_*): absorption low / mid / high
// and scattering. Mid / high are the room model's per physical material (kAbs*); low from
// typical tables (glass panes and wood panelling absorb bass, masonry does not); scattering:
// smooth hard surfaces low, soft / irregular ones high (furniture is geometry here).
enum BClassId : U8 { BC_WOOD = 0, BC_STONE, BC_METAL, BC_GLASS, BC_FLESH, BC_PLASTIC, BC_RUBBER, BC_TERRAIN,
                     BC_FOLIAGE, BC_WATER, BC_SNOW, BC_SNOW_PACKED, BC_COUNT };
struct BClass { F32 low, mid, hf, scatter; };
constexpr BClass kBClass[BC_COUNT] = {
    {0.12f, kAbsWood.mid,    kAbsWood.hf,    0.15f},   // wood (the SL default; light / unknown too)
    {0.02f, kAbsStone.mid,   kAbsStone.hf,   0.10f},
    {0.03f, kAbsMetal.mid,   kAbsMetal.hf,   0.05f},
    {0.18f, kAbsGlass.mid,   kAbsGlass.hf,   0.05f},
    {0.25f, kAbsFlesh.mid,   kAbsFlesh.hf,   0.60f},
    {0.08f, kAbsPlastic.mid, kAbsPlastic.hf, 0.10f},
    {0.15f, kAbsRubber.mid,  kAbsRubber.hf,  0.30f},
    {0.15f, kAbsTerrain.mid, kAbsTerrain.hf, 0.50f},
    {0.40f, kAbsFoliage.mid, kAbsFoliage.hf, 0.80f},
    {0.01f, kAbsWater.mid,   kAbsWater.hf,   0.05f},   // water (#953: the room rays score it the same)
    {0.45f, 0.75f,           0.90f,          0.70f},   // #930 fresh snow (dry; fillRequest folds the wetness in)
    {0.15f, 0.30f,           0.40f,          0.40f},   // #930 packed snow
};
static_assert(BC_COUNT <= LLVKAcousticsRequest::kBounceClasses, "bounce material classes");
static_assert(BC_SNOW == 10 && BC_SNOW_PACKED == 11, "clean_audio_rt.comp ACC_SNOW / ACC_SNOW_PACKED");

// ---- #930 LYING SNOW (AudioAcousticsSnowAbsorb, default 1; 0 = ignore snow) ----
// Fresh snow is one of the most absorbent natural surfaces (a porous ice skeleton, ~90 % air):
// ~0.45 at 250 Hz, ~0.75 at 1 kHz, ~0.9 at 5 kHz for a layer >= ~10 cm (thinner layers are a
// fraction of that; a dusting does nothing). Packing closes the pores (packed / trodden snow
// ~0.15 / 0.3 / 0.4), and melt water fills them: wet / slushy snow sits between snow and water.
// WHERE: the probe looks the hit up in the weather's snow depth grids (clean_audio_rt.comp
// acSnowHit, AC_SNOW): snow only where it really lies -- the top faces of the grid's columns
// and the faces under a drift; never a ceiling, a wall above the drift line, or ground under
// a roof / canopy. Per hit (w, packed): w = the snow share (depth ramp 3 .. 10 cm x the
// strength), packed = the compaction proxy (the depth against the global, undisturbed level:
// footprints / tracks press it to ~0.2-0.3 of it). Wetness is the weather's (SLWeather wetness:
// rain on the snow, melt water), the same everywhere.
// EFFECTS (all from the per-hit absorption; nothing else changes): the room model's surface
// absorption (shorter decay, lower wet, weaker early reflections where the hits are snowy --
// mostly the ground outdoors), #897 boundary bass (snow is not a rigid boundary), #858 paths
// reflecting off snow, and in the bounce pass every hit (decay, early reflections, #683 early
// gain) through the snow classes below. Weather audio (rain / wind beds) is untouched.
// FALLING SNOW (heavy snowfall only, x the strength): a little extra air absorption per band
// (the flakes scatter / absorb the highs a little), subtle: +0.01 dB/m at 5 kHz at full intensity.
struct SnowAbs { F32 low, mid, hf, scatter; };
constexpr SnowAbs kSnowFresh  = {0.45f, 0.75f, 0.90f, 0.70f};
constexpr SnowAbs kSnowPacked = {0.15f, 0.30f, 0.40f, 0.40f};
constexpr SnowAbs kSnowSlush  = {0.10f, 0.20f, 0.25f, 0.20f};   // (water 0.01 / 0.01 / 0.02)
constexpr F32 kSnowBndHard = 0.15f;         // #897 hardness of fresh snow (packed: 0.45, slush 0.4)
constexpr F32 kSnowAirDbPerM[3] = {0.0003f, 0.002f, 0.01f};   // falling snow at full intensity (extra)
F32 sSnowWet = 0.f;                         // the weather's wetness while snow lies (fillRequest)
F32 sSnowFall = 0.f;                        // heavy snowfall share 0..1 x the strength (fillRequest)

// The lying snow's absorption at compaction `pack` and the current wetness.
SnowAbs snowAbs(F32 pack)
{
    auto mixs = [](const SnowAbs& a, const SnowAbs& b, F32 t)
    {
        return SnowAbs{a.low + (b.low - a.low) * t, a.mid + (b.mid - a.mid) * t, a.hf + (b.hf - a.hf) * t,
                       a.scatter + (b.scatter - a.scatter) * t};
    };
    return mixs(mixs(kSnowFresh, kSnowPacked, llclamp(pack, 0.f, 1.f)), kSnowSlush, llclamp(sSnowWet, 0.f, 1.f));
}
F32 snowBndHard(F32 pack)
{
    const F32 h = kSnowBndHard + (0.45f - kSnowBndHard) * llclamp(pack, 0.f, 1.f);
    return h + (0.4f - h) * llclamp(sSnowWet, 0.f, 1.f);
}
// #858: a path reflecting / scattering at a snowy point loses what the snow absorbs beyond a
// plain (wood) surface: amplitude x sqrt(1 - excess) at mid, the HF ratio likewise.
void snowPathLoss(F32 w, F32 pack, F32& m, F32& h)
{
    m = 1.f; h = 1.f;
    if (!(w > 0.f)) return;
    const SnowAbs a = snowAbs(pack);
    const F32 em = llclamp(1.f - w * std::max(a.mid - 0.07f, 0.f), 0.05f, 1.f);
    const F32 eh = llclamp(1.f - w * std::max(a.hf - 0.09f, 0.f), 0.05f, 1.f);
    m = std::sqrt(em);
    h = std::sqrt(std::min(eh / em, 1.f));
}

// ---- #953 WATER: THE AIR / WATER BOUNDARY AND THE UNDERWATER LISTENER ----
// MEDIUM: which side of the water surface a point is on -- its region's water height
// (LLViewerRegion::getWaterHeight; outside every region the agent's, as the void water is drawn).
// SL water is the regions' flat water planes, and the TLAS water instances are built from the
// same heights, so the height test IS the traced surface without a ray; it is also exactly the
// renderer's own underwater rule (LLViewerCamera::cameraUnderWater). A sealed air pocket below
// the water level (an underwater dome) counts as water, as it renders. Blended over
// +-kMediumBand of the surface (a float bobbing on it does not flip), smoothed kMediumTau.
//  * BOUNDARY LOSS (AudioAcousticsWaterBoundaryLoss dB, + AudioAcousticsWaterBoundaryLossHF more
//    at HF): a source and the listener on OPPOSITE sides of the surface. The interface transmits
//    ~0.1 % of the energy (normal incidence T = 4 Z1 Z2 / (Z1 + Z2)^2 ~ 0.0011: -29.5 dB), more
//    lost at high frequencies in practice (ripples, bubbles, grazing paths beyond the critical
//    angle). Through the EXISTING per-source filter path, after everything else (occlusion,
//    #858 paths, #683 early gain): the direct gain, its HF, the listener-room send and the zone
//    send -- every path between the two sides crosses the surface, so the shares that route
//    round a wall do not escape it. Share x = s (1 - l) + l (1 - s) of the two in-water shares;
//    the loss is x times the setting in dB: exactly 0 dB on the same side. A source starts at
//    its value (no fade-in from full level), and a voice not answered yet takes it too.
//  * UNDERWATER LISTENER (AudioAcousticsUnderwater): the ear in the water engages a profile,
//    crossfaded over kUwTau (setting toggles too):
//      - the sources in AIR: the boundary loss above (strong HF loss);
//      - HEARING: an ear in water hears by bone conduction -- every in-world sound a little
//        duller (kUwHearHf on the direct HF and the reverb slots' GAINHF); mild, the
//        boundary already took the air sources' highs;
//      - REVERB: the listener's room is measured as always (now with the surface: from below a
//        pressure-release boundary, as reflective as from above) but sound travels ~4.3 x faster
//        (kSpeedWater): a decay time ~ 1 / c (Sabine / Eyring; the bounce measurement's times
//        too), reflection / late delays ~ 1 / c, and a denser echo pattern. Zone reverbs take the
//        hearing part only (their sources' rooms are not necessarily in the water).
//      - PROPAGATION: inverse-distance spreading is the same in water and water absorbs far
//        less than air, so the legacy x5 distance rolloff underwater (llvieweraudio.cpp, a game
//        muffle) is NOT stacked on top: ownsUnderwater() keeps it at 1 while this runs. Doppler
//        and the bounce pass still use the speed in air (not modelled).
constexpr F32 kMediumBand = 0.1f;        // m either side of the surface: blended
constexpr F32 kMediumTau = 0.08f;        // s: a source / the ear crossing the surface (no zipper)
constexpr F32 kUwTau = 0.2f;             // s: the underwater profile's crossfade
constexpr F32 kSpeedWater = 1480.f;      // m/s
constexpr F32 kUwHearHf = 0.6f;          // underwater hearing: HF x (~ -4.4 dB at 5 kHz)
constexpr F32 kUwDensity = 1.f;          // the underwater reverb's echo density

// 0 in air .. 1 in water at agent position p (see MEDIUM).
F32 inWater(const LLVector3& p)
{
    LLViewerRegion* rg = LLWorld::getInstance()->getRegionFromPosAgent(p);
    if (!rg) rg = gAgent.getRegion();
    if (!rg) return 0.f;
    const F32 depth = rg->getWaterHeight() - p.mV[VZ];
    return depth <= -kMediumBand ? 0.f : depth >= kMediumBand ? 1.f : (depth + kMediumBand) / (2.f * kMediumBand);
}

// Smoothed toward a target, landing exactly on it (0 / 1 stay exact: no residual loss).
void glideTo(F32& v, F32 target, F32 a)
{
    v += (target - v) * a;
    if (std::fabs(v - target) < 1e-3f) v = target;
}

// #953: the boundary loss of a source (in-water share src_w) for the listener's share lis_w:
// mid gain and the extra HF gain (both 1 on the same side).
void boundaryLoss(F32 src_w, F32 lis_w, F32 db, F32 db_hf, F32& g, F32& g_hf, F32* x_out = nullptr)
{
    const F32 x = llclamp(src_w * (1.f - lis_w) + lis_w * (1.f - src_w), 0.f, 1.f);
    if (x_out) *x_out = x;
    g = x > 0.f ? std::pow(10.f, -db * x / 20.f) : 1.f;
    g_hf = x > 0.f ? std::pow(10.f, -db_hf * x / 20.f) : 1.f;
}

// ---- #897 BOUNDARY BASS ----
// Near a hard surface low frequencies add up in phase with their reflection: about +3 dB per
// close boundary (a wall +3, an edge +6, a corner +9), fading out once the surface is farther
// than about a quarter wavelength -- at both ends: the listener's surroundings and the source's.
// The bounce pass does NOT give this: it is energy (incoherent) and lands in the shared reverb
// slot as early reflections (a delayed, diffused, broadband cluster); nothing it drives changes
// the DIRECT sound's low end (EAX GAINLF can only cut). So it is its own small effect:
//  * DISTANCES from rays already traced -- the listener's 64 room rays and each source's #857
//    probe rays: per world axis (+-x, +-y, +-z) the nearest surface straight out (rays within
//    ~37 degrees of the axis, distance t cos -- exact for a surface facing the axis, the usual SL
//    build; a rotated wall reads a little farther, in two bins), with its HARDNESS (stone / metal
//    1, wood 0.75, glass 0.65, terrain 0.7, foliage 0.2, ...; the hardness hint pulls hard),
//    smoothed (closer: fast, farther: slower). Probe rays start at the emitter's own size, so a
//    big emitter does not see walls closer than that.
//  * BOOST per end = 3 dB x sum over the axes of hardness x fade(distance), at most 9 dB; the
//    fade is full inside half a quarter wavelength of the shelf (kBassShelfHz) and gone at 1.5
//    quarter wavelengths. Per source: listener + source (stale source probe: listener only),
//    capped at kAcousticsBassMaxDb, x AudioAcousticsBoundaryBass, smoothed.
//  * APPLIED by the engine as a parallel send into one EQUALIZER slot (a low shelf, the only EFX
//    route that can BOOST: per-source filters only cut), with the dry path's distance attenuation
//    and a direct-path trim that keeps the mids unchanged (LLAudioEngine::setAcousticsBass).
//    Channel voices and the #892 speakers; not thunder (a far source in the open).
constexpr F32 kBndCone = 0.8f;          // |cos| to the axis
constexpr F32 kBndFar = 10.f;           // m: an escaped ray in the cone
constexpr F32 kBndAttack = 0.15f;       // s: a surface coming closer
constexpr F32 kBndRelease = 0.5f;       // s: going away
constexpr F32 kBassTau = 0.3f;          // s: a source's boost
constexpr F32 kBassQuarter = kSpeed / (4.f * LLAudioEngine::kAcousticsBassShelfHz);   // m (~0.48 at 180 Hz)
constexpr F32 kBassPerBoundary = 3.f;   // dB
constexpr F32 kBassMaxPerEnd = 9.f;     // dB (a corner)
struct Boundary
{
    bool valid = false;
    F32  d[6] = {kBndFar, kBndFar, kBndFar, kBndFar, kBndFar, kBndFar};   // -x +x -y +y -z +z (m)
    F32  h[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};                           // hardness 0..1
};

// A smoothed room estimate (the listener's, a source's, a zone's).
struct Room
{
    F32 enclosure = 0.f;       // side / up hit fraction (wall-weighted)
    F32 roof = 0.f;            // up hit fraction
    F32 room_size = 10.f;      // mean wall / ceiling distance, gated by enclosure (m)
    F32 surf_mid = kAbsWood.mid;   // mean SURFACE absorption of the hit rays
    F32 surf_hf = kAbsWood.hf;
    F32 clutter = 0.f;         // geometric clutter 0..1
    F32 escape = 0.f;          // fraction of ALL rays that hit nothing (open area)
};

// What the last reverb computation derived (log).
struct RoomDerived
{
    F32 eff_mid = 0.f, eff_hf = 0.f;   // effective absorption (surfaces + clutter, biased)
    F32 tot_mid = 0.f, tot_hf = 0.f;   // total absorption (open area included)
    F32 stat_decay = 0.f, stat_wet = 0.f;   // the statistical model's own (before the #683 phase 1 blend)
    F32 blend = 0.f, blend_decay = 0.f;     // how much of the measured values went in
    F32 stat_room_k = 1.f;                  // #930: the statistical wet's room-constant factor (roomConstK)
    F32 meas_wet = 0.f, meas_cont = 0.f, meas_room_k = 1.f;   // #930: the measured wet, its containment, its room constant
    F32 meas_open = 0.f, meas_a_area = 0.f, meas_a_energy = 0.f;   // #930: ... the open share and the two absorptions behind it
};

// ---- #683 PHASE 1: MEASURED REVERB FROM THE BOUNCE PASS (AudioAcousticsBounce) ----
// The GPU traces AudioAcousticsBounceRays rays from the listener (and AudioAcousticsBounceSourceRays
// from each of up to AudioAcousticsBounceSources sources in ANOTHER room, #857) for up to
// AudioAcousticsBounces bounces each against our TLAS (clean_audio_rt.comp BOUNCE PASS), on the
// AudioAcousticsRoomInterval cadence one frame after the room rays. Per hit: a 3-band absorption
// (LOW ~250 Hz / MID ~1 kHz / HIGH ~5 kHz -- EAX's LF / HF references) and a scattering
// coefficient by material class (the table below: the room model's mid / HF absorption per
// physical material, a low band, + the room's clutter, AudioAcousticsAbsorptionBias and
// kBounceUnmodelled), air absorption per band, diffuse rain + specular returns into a 3-band
// ECHOGRAM (5 ms bins to 1.28 s), early (orders 1-2, 1 ms bins) and late (orders >= 3)
// arrivals with their energy-weighted directions, and the ENERGY BALANCE (arriving at hits,
// absorbed, escaped into open area, lost to the air). Every answer's sums are added to
// exponentially forgotten accumulators (kBounceTau of real time: at 60 fps and the defaults
// ~15 answers, ~7700 listener rays per tau), and analysed (analyseBounce):
//  * DECAY per band, two estimators:
//      RATE -- the population's measured loss rate: energy-weighted absorption per reflection
//        a, escape probability per segment p (both from the balance), the measured mean free
//        time t (hit segments' length / hits / c), air m: lambda = -ln((1 - p)(1 - a)) / t + m c,
//        T60 = ln(10^6) / lambda. A ray-traced Eyring: the real geometry decides which surfaces
//        get hit how often, how long the paths are and how much leaks out of the openings.
//        It does not care that a ray stops after B bounces.
//      FIT -- the echogram, survival-corrected for rays the bounce cap ended (cap counts; the
//        window ends where half are gone), its tail beyond the window extrapolated at the RATE,
//        Schroeder backward-integrated, and a least-squares line on the energy decay curve from
//        -5 to -25 dB (T20; -5 to -35 dB = T30 when the data covers 40 dB), or over whatever the
//        data covers (>= 3 dB, from -1 dB when it covers less than 12 dB).
//    With 6-8 bounces a reverberant room loses only a few dB before the cap (a metal hall ~1 dB
//    per bounce) -- a T20 / T30 needs a dead or open space. So the decay is the RATE, pulled
//    toward the FIT by its weight smooth(3..25 dB span) x smooth(r^2 0.85..0.98): the fit only
//    takes over where the data actually shows the decay. Both are logged.
//  * CONTAINMENT = 1 - escaped / (escaped + absorbed + air) at mid: the share of the sound energy
//    that stays in the space until walls / air take it (a closed room 1, open ground ~0.1). The
//    steady-state reverberant level goes with it (relative to the same room closed), so it sets
//    the measured WET: level x smooth(0.15..0.75) x (a dead room's tail quieter, as the model).
//    Little returning energy = dry: outdoors stays dry.
//  * EARLY REFLECTIONS: the order 1-2 arrivals -- REFLECTIONS_DELAY = their 10 % onset,
//    REFLECTIONS_GAIN from their energy against the late energy (kReflCal, the late gain fixed
//    at 1.26 as the model), REFLECTIONS_PAN = their energy-weighted direction (length = how
//    concentrated: a wall beside you pulls it there). LATE_REVERB_DELAY = the orders >= 3 onset
//    after the first reflection; LATE_REVERB_PAN = the late energy's direction when it is
//    concentrated (a wall on one side in the open), else centred. GAINHF / GAINLF = the first
//    80 ms' HF / LF energy against mid.
//  * BLEND: the statistical model above stays the fallback and the floor. Each value moves
//    toward the measured one by the measurement's confidence -- statistical (effective rays in
//    the window: 200 .. 1200) for wet, times enough hits (30 .. 300) for the decay, delays,
//    gains and pans -- fading out 1..3 s after the last answer (bounce off, RT standing down).
// LISTENER-CENTRIC: the receiver is the listener (sound from where you stand, heard where you
// stand); the reverb slot is shared by every source, as before. SOURCE ROOMS (#857): a source in
// another room gets its own source-centric set (AudioAcousticsBounceSources most overdue of the
// AudioAcousticsSourceRooms most audible, each at most every kBounceSrcEvery), the same analysis
// on its own accumulator (kSrcBouncePool, LRU), and its zone's reverb blends toward its members'
// measurements.
struct BounceAcc
{
    F64 last_at = 0.0;
    U32 bounces = 6;   // the cap of the newest answer (late-energy completion)
    F32 rays = 0.f, hits = 0.f, escapes = 0.f, capped = 0.f, nee = 0.f, spec = 0.f, seg_len = 0.f;
    F32 hist[3][LLVKAcousticsBounceSet::kBins] = {};
    F32 early[LLVKAcousticsBounceSet::kEarlyBins] = {};
    F32 late[LLVKAcousticsBounceSet::kLateBins] = {};
    F32 cap[LLVKAcousticsBounceSet::kCapBins] = {};
    F32 edir[3] = {0.f, 0.f, 0.f}, ldir[3] = {0.f, 0.f, 0.f};
    F32 e_pre[3] = {0.f, 0.f, 0.f}, e_abs[3] = {0.f, 0.f, 0.f}, e_esc[3] = {0.f, 0.f, 0.f}, e_air[3] = {0.f, 0.f, 0.f};
};

// One analysed bounce measurement (the listener's, a source's, a zone's blend of its members').
struct Measured
{
    bool  valid = false;
    F64   at = 0.0;                 // analysed (real time)
    F32   conf = 0.f;               // statistical convergence 0..1 (wet / containment)
    F32   conf_decay = 0.f;         // ... with enough hits (decay, ratios, delays, gains, pans)
    F32   t60[3] = {1.f, 1.f, 1.f};   // per band (low / mid / high): RATE pulled toward FIT
    F32   t_rate[3] = {0.f, 0.f, 0.f};
    F32   t_fit[3] = {0.f, 0.f, 0.f};     // 0 = no fit
    F32   fit_r2[3] = {0.f, 0.f, 0.f};
    F32   fit_span[3] = {0.f, 0.f, 0.f};  // dB fitted
    F32   fit_w[3] = {0.f, 0.f, 0.f};     // the fit's weight in t60
    F32   covered[3] = {0.f, 0.f, 0.f};   // dB of decay the data covers before the rate tail takes over
    F32   containment = 0.f, absorbent = 0.f, mfp = 0.f;
    F32   alpha_mid = 0.f;          // #930: the bounce hits' ENERGY-weighted absorption per reflection (mid; incl. unmodelled)
    F32   refl_gain = 0.3f, refl_delay = 0.02f, late_delay = 0.02f;
    F32   gain_hf = 0.89f, gain_lf = 1.f;
    LLVector3 early_dir, late_dir;  // unit (zero: none)
    F32   early_conc = 0.f, late_conc = 0.f;
    F32   e_early = 0.f, e_late = 0.f;    // per ray
    F32   rays = 0.f, hits_per_ray = 0.f, esc_frac = 0.f, cap_frac = 0.f, nee_per_ray = 0.f, spec_per_ray = 0.f;
};

// One set of room-style rays (the listener's 64, or one source's probe rays).
struct RaySet
{
    U32 n = 0;
    F32 dist[kRoomRays];       // < 0 miss
    F32 dir_z[kRoomRays];
    F32 dir_x[kRoomRays];      // (#897: the full direction)
    F32 dir_y[kRoomRays];
    U32 inst[kRoomRays];
    F32 hint[kRoomRays];
    F32 clutter[kRoomRays];    // < 0 none
    U8  kind[kRoomRays];
    F32 snow[kRoomRays];       // #930: lying-snow share 0..1 (x the strength)
    F32 snow_pack[kRoomRays];  // #930: ... how packed 0..1
};

// One answer's instantaneous room measurement.
struct RoomSample
{
    bool ok = false;           // any non-floor ray
    F32  enc = 0.f, roof = 0.f, esc = 0.f, hit_size = 0.f, size = 0.f;
    bool have_abs = false;
    F32  am = 0.f, ah = 0.f;
    bool have_clut = false;
    F32  clut = 0.f;
    F32  mat[LLCleanAcoustics::ROOF_MAT_COUNT] = {0.f, 0.f, 0.f, 0.f};
    F32  mat_n = 0.f;
    // #897: per world axis the nearest surface straight out (m; < 0 = no ray in that cone) + hardness
    F32  bnd_d[6] = {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f};
    F32  bnd_h[6] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
};

// Room-ray surface counters (the listener's log window).
struct HitCounts
{
    U32 hits = 0, hard = 0, soft = 0, terrain = 0, foliage = 0, glass = 0, rays = 0, escapes = 0;
    U32 water = 0;             // #953
    F32 snow = 0.f;            // #930: summed snow shares of the hits
};

// What each path query of a request asked (answers come back in the same order).
enum QType : U8 { Q_PROBE = 0, Q_GUIDE_L, Q_GUIDE_S, Q_LEG, Q_JOIN };
struct QMeta
{
    U8        type = Q_PROBE;
    U8        legs = 0;        // Q_LEG: the path's leg count
    U16       src = 0;         // index into Pending::ids
    U32       uid = 0;         // Q_LEG: the path's uid
    F32       dir_z = 0.f;     // Q_PROBE: the ray's world z (floor rays)
    F32       dir_x = 0.f;     // Q_PROBE: ... x / y (#897 boundaries)
    F32       dir_y = 0.f;
    LLVector3 aux;             // Q_JOIN: the source-side point P2
};

struct Pending
{
    U32       serial = 0;
    U32       n = 0;
    LLUUID    ids[kMaxSrc];
    LLVector3 pos[kMaxSrc];    // agent space, as requested
    LLVector3 listener;
    U32       nq = 0;
    QMeta     meta[kMaxQueries];
    U32       eg_n = 0;                                      // #683 early-gain sets in this request ...
    U32       eg_k[LLVKAcousticsRequest::kMaxBounceSets];    // ... their source indices (into ids)
};

// #858 one cached path listener -> p1 (-> p2) -> source.
struct Path
{
    U32       uid = 0;         // 0 = empty slot
    U8        bounces = 1;
    LLVector3 p1, p2;
    F32       snow_m = 1.f;    // #930: amplitude kept at a snowy reflection point (mid) ...
    F32       snow_h = 1.f;    // ... and the HF ratio on top
    F64       found = 0.0;
    F64       last_ok = 0.0;   // last validation (or discovery) that saw every leg clear
};

// A point a source sees that the listener does not (two-bounce candidate P2).
struct FarPt
{
    LLVector3 p;
    F32       d = 0.f;         // distance from its origin (farther = more likely through an opening)
    F64       at = 0.0;        // 0 = empty
};

// Everything known about one traced source (keyed by its id).
struct SrcState
{
    // occlusion (the direct ray)
    bool  occ_valid = false;   // answered at least once
    F32   occ = 0.f;           // smoothed blocked fraction 0..1
    F64   seen = 0.0;          // last request / answer that traced it
    F32   prio = 0.f;          // audibility proxy (gain / distance), last request
    LLVector3 pos;             // agent space, last request
    F32   margin = 0.5f;       // its ray end margin (the emitter's own size)
    bool  top = false;         // among the kTopOccluded most audible occluded sources (last request)
    // #857 own room (probe)
    bool  room_valid = false;
    Room  room;
    F32   couple = 0.f;        // smoothed fraction of probe end points the listener sees
    F32   wet = 0.f;           // its room's wet level (reverbFromRoom)
    F64   probe_req = 0.0;     // last time it was put in a request
    F64   probe_at = 0.0;      // last probe answer
    F64   first_probe_at = 0.0;   // first probe answer (the probe rule ramps in from it)
    // #858 path cache + schedule
    Path  paths[kCachePaths];
    FarPt far_s[kSrcFar];
    U8    val_rot = 0;         // first path the next validation pass starts at
    F64   val_req = 0.0;       // last validation request / answer
    F64   val_at = 0.0;
    F64   disc_req = 0.0;      // last discovery request / answer
    F64   disc_at = 0.0;
    F64   diff_at = 0.0;       // last room answer whose shadow rays traced it (rotation)
    // derived each update
    F32   q = 0.f, q_hf = 0.f; // smoothed indirect strength (setting applied)
    F32   rho = 0.f;           // best valid path's detour ratio (log)
    U32   n_valid = 0;         // valid cached paths (log)
    LLVector3 ind_dir;         // apparent direction of the valid paths (unit)
    bool  ind_dir_valid = false;
    F32   conc = 0.f;          // how concentrated their directions are (0 scattered .. 1 one opening)
    F32   same = 1.f;          // same-room weight
    F32   pan_w = 0.f;         // smoothed pan weight
    LLVector3 pan_dir;         // smoothed apparent direction (unit)
    bool  pan_valid = false;
    // zone
    S32   zone = -1;           // assigned zone (the send's slot)
    F32   zone_fade = 0.f;     // 0..1: ramps out before a retarget, in after
    F32   zone_want = 0.f;     // target zone send level
    F32   zone_send = 0.f;     // smoothed
    // applied values (also the #892 query)
    F32   out_gain = 1.f, out_hf = 1.f, out_send = 1.f, out_send_hf = 1.f, out_zone_hf = 1.f;
    // #683 phase 1: its own room measured by a source-centric bounce set (other-room sources)
    Measured meas;
    F64   bounce_at = 0.0;     // last bounce answer for it
    // #897 boundary bass: its surroundings (probe rays) and its smoothed boost (dB)
    Boundary bnd;
    F32   bass_db = 0.f;
    // #683 early gain: reflected energy reaching the listener from it (early-gain sets)
    F32   eg_num[3] = {0.f, 0.f, 0.f};   // forgotten sums of window energy / free-field energy ...
    F32   eg_den = 0.f;                  // ... and of rays (ratio = num / den)
    F64   eg_acc_at = 0.0;               // last accumulation (= last answer)
    F64   eg_disp = 0.0;                 // last dispatch (in flight until answered or kEarlyHold)
    F32   eg_ratio[3] = {0.f, 0.f, 0.f}; // early / free-field direct energy per band (log)
    F32   eg_mult = 1.f;                 // smoothed level multiplier on today's direct gain
    F32   eg_hf = 1.f;                   // smoothed multiplier on today's HF filter
    F32   eg_share = 0.f;                // the added share of the arriving energy (log)
    F32   eg_rays_win = 0.f;             // rays answered in the log window
    // (log window) per-ray diagnostics of its early-gain sets: hits, shadow rays that saw the
    // listener (diffuse arrivals), specular near-passes; the emitter start radius, receiver radius
    // and window last dispatched / applied; a short label (what kind of emitter, its sound)
    F32   eg_hits_win = 0.f, eg_nee_win = 0.f, eg_spec_win = 0.f;
    F32   eg_start = 0.f, eg_rr = 0.f, eg_window = 0.f;
    bool  eg_cached = false;             // seeded from the spatial cache (log)
    bool  eg_jump = false;               // a new sound seeded from the cache: no attack ramp
    F64   first_seen = 0.0;              // first request that traced it
    char  label[24] = {0};
    F32   out_boost = 1.f;               // applied: the source-gain boost (>= 1)
    // #953 the water: its in-water share (smoothed) and the boundary share applied (log)
    bool  med_valid = false;
    F32   med = 0.f;
    F32   med_x = 0.f;
};

struct Zone
{
    bool  active = false;
    bool  fresh = false;       // claimed this update: its room is the claimant's
    Room  room;
    F32   f[4] = {0.f, 0.f, 0.f, 0.f};
    F64   last_member = 0.0;
    U32   members = 0;
    LLAcousticsReverb rv;      // last pushed (log)
    Measured meas;             // #683 phase 1: its members' measurements, blended
};

struct State
{
    LLAudioEngine* engine = nullptr;
    bool   active = false;        // the engine's EFX path is on
    bool   failed = false;        // enabling failed (no EFX): latched until the setting goes off
    bool   warned_rt = false;
    Pending ring[kRing];
    U32    last_serial = 0;       // newest answer consumed
    F64    last_answer = 0.0;     // when
    U32    last_room_serial = 0;  // newest room answer consumed (room rays arrive every AudioAcousticsRoomInterval frames)
    F64    last_room_answer = 0.0;
    F64    last_update = 0.0;
    bool   room_valid = false;
    Room   L;                     // the listener's room, smoothed
    // Smoothed material shares of the solid UPWARD hits (wood / metal / glass / tent, sum 1):
    // which rain-on-roof bed the weather audio plays (#783). Held while nothing is overhead.
    F32    roof_mat[LLCleanAcoustics::ROOF_MAT_COUNT] = {1.f, 0.f, 0.f, 0.f};
    F32    hit_size = 10.f;       // smoothed raw mean hit distance (log only; room_size is gated)
    RoomDerived derived;          // the listener reverb's (log)
    F64    reverb_push = 0.0;
    LLAcousticsReverb reverb;     // last pushed
    F32    level = 0.6f;          // AudioAcousticsReverbLevel / Bias, latched each update
    F32    bias = 0.f;
    std::unordered_map<LLUUID, SrcState> src;
    U32    src_peak = 0;
    // #858 the listener's far points (room-ray hits much farther than the room: openings)
    FarPt  lfar[kFarPoints];
    U32    path_uid = 0;
    U32    rng = 0x9E3779B9u;
    // #857 zones
    Zone   zones[kMaxZones];
    U32    zones_avail = 0;       // the engine's zone count
    U32    zones_wanted = 0;      // what was last asked of it
    // log window
    F64    next_log = 0.0;
    F64    win_start = 0.0;
    U32    answers = 0;
    U32    frames_old_sum = 0;
    U32    traced = 0;            // sources in the newest request
    U32    probe_n = 0;           // probes in the newest request
    U32    diff_n = 0;            // diffraction sources in the newest request
    U32    q_probe = 0, q_leg = 0, q_guide = 0, q_join = 0;   // the newest request's path queries by kind
    U64    q_sum = 0;             // path queries requested in the window
    U32    q_frames = 0;
    HitCounts hc;
    U32    occ_glass = 0;         // sources whose newest ray crossed glass / a hard screen only (window)
    U32    win_probes = 0;        // source probes consumed
    U32    win_probe_vis = 0;     // ... their rays whose end point the listener saw
    U32    win_probe_rays = 0;
    U32    win_nee = 0;           // diffraction source evaluations (room answers x sources)
    U32    win_nee_found = 0;     // ... that found a path
    U32    win_valid_ok = 0;      // cached paths re-validated
    U32    win_valid_lost = 0;    // ... dropped by a failed leg
    U32    win_expired = 0;       // ... dropped unvalidated
    U32    win_found = 0;         // new paths cached
    U32    win_found_2b = 0;      // ... of them two-bounce
    // #683 phase 1 bounce pass
    bool   bounce_on = false;     // AudioAcousticsBounce, latched by fillRequest
    BounceAcc LB;                 // the listener's accumulator ...
    Measured  LM;                 // ... and its analysis
    struct SrcBounce
    {
        LLUUID    id;
        BounceAcc acc;
        F64       used = 0.0;
    };
    SrcBounce sb[kSrcBouncePool];   // source-centric accumulators (LRU)
    U32    last_bounce_serial = 0;
    U32    bounce_src_n = 0;      // source sets in the newest request
    // #683 early gain
    bool   early_on = false;      // AudioAcousticsEarlyGain, latched by fillRequest
    U32    early_src_n = 0;       // early-gain sets in the newest request
    U32    eg_last_disp = 0;      // the request serial whose dispatch was last noted
    U32    win_early_sets = 0;    // early-gain sets consumed in the log window
    U32    win_bounce = 0;        // bounce answers in the log window
    U32    win_bounce_src = 0;    // source sets consumed
    F32    win_bounce_rays = 0.f; // listener rays traced
    U32    bounce_wg = 0, bounce_frames_old = 0, bounce_max = 0;
    // #897 boundary bass
    Boundary LBnd;                // the listener's surroundings (room rays)
    bool   bass_wanted = false;   // asked the engine for the EQ slot
    bool   bass_avail = false;    // ... and it exists
    // #953 the water
    bool   med_valid = false;
    F32    lmed = 0.f;            // the ear's in-water share, smoothed (the boundary loss)
    F32    uw = 0.f;              // the underwater profile's blend (AudioAcousticsUnderwater), smoothed
    bool   uw_on = true;          // AudioAcousticsUnderwater, latched by update
};

State& st()
{
    static State s;
    return s;
}

// Request serials keep counting across engine restarts: the renderer keeps the newest answer
// by serial, so a restarted counter would have its answers ignored.
U32 sSerial = 0;

// Channel-less sources (traceExtraSource): #783 thunder, #892 speakers (up to 32). Keyed, bounded.
// kMaxSrc (64) = 30 channels + kMaxExtra (34 = 32 speakers + thunder and spare): the speakers
// never take a channel voice's ray.
struct Extra
{
    LLUUID    key;
    LLVector3 pos;
    F64       until = 0.0;     // traced while now < until
};
constexpr U32 kMaxExtra = kMaxSrc - LL_MAX_AUDIO_CHANNELS;   // 34
static_assert(kMaxExtra >= 32 + 2, "ParcelAudioMaxSpeakers (32) + thunder must fit the extra sources");
static_assert(LL_MAX_AUDIO_CHANNELS + kMaxExtra <= kMaxSrc, "channel voices + extra sources must fit the ray budget");
Extra sExtra[kMaxExtra];
constexpr F64 kExtraHold = 0.5;

F32 smooth01(F32 e0, F32 e1, F32 x)
{
    const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

F32 lerpf(F32 a, F32 b, F32 t) { return a + (b - a) * t; }

F32 follow(F32 tau, F32 dt) { return dt <= 0.f ? 1.f : 1.f - std::exp(-dt / tau); }

// Normalised blend of two unit vectors (falls back to b when they cancel).
LLVector3 nlerp(const LLVector3& a, const LLVector3& b, F32 t)
{
    LLVector3 v = a + (b - a) * t;
    const F32 len = v.length();
    return len > 1e-3f ? v / len : b;
}

// 0 open .. 1 enclosed (enclosure 0.95+ = a room: size and clutter count in full; a room
// with a door / window gap still ~0.95 at 0.85).
F32 enclosedWeight(F32 enclosure) { return smooth01(0.3f, 0.95f, enclosure); }

// #930 ROOM CONSTANT: the steady-state reverberant level goes with (1 - a) / a (Sabine's room
// constant R = S a / (1 - a), E_rev ~ 4 / R) for the mean absorption per reflection a. The wet
// models below already normalise by an open area (containment / open_level) and only took the
// surfaces' absorption in as a mild "absorbent" trim (>= 0.75): fine for rooms, but a space whose
// surfaces swallow most of the sound -- deep fresh snow (a ~ 0.85-0.95 with the unmodelled share),
// dense foliage -- kept most of its wet. This factor is 1 up to a0 (a fairly dead furnished room:
// nothing changes there) and falls with the room constant beyond it (a 0.95 -> ~0.06).
F32 roomConstK(F32 a, F32 a0)
{
    a = llclamp(a, 0.01f, 0.99f);
    return llclamp(((1.f - a) / a) / ((1.f - a0) / a0), 0.f, 1.f);
}

// ---- #683 phase 1: bounce-pass helpers ----

F32 airPerM(U32 band) { return (kAirDbPerM[band] + sSnowFall * kSnowAirDbPerM[band]) / 4.3429448f; }   // (+ #930 falling snow)

// The bounce class of a physical material code (LL_MCODE_*).
U8 classOfCode(S32 code)
{
    switch (code)
    {
    case LL_MCODE_STONE:   return BC_STONE;
    case LL_MCODE_METAL:   return BC_METAL;
    case LL_MCODE_GLASS:   return BC_GLASS;
    case LL_MCODE_FLESH:   return BC_FLESH;
    case LL_MCODE_PLASTIC: return BC_PLASTIC;
    case LL_MCODE_RUBBER:  return BC_RUBBER;
    default:               return BC_WOOD;   // wood, light, unknown
    }
}

// INSTANCE -> MATERIAL CLASS CACHE for the GPU (bounded: kMatSlots, 8-way open addressing, the
// oldest in the window gives way). Fed by every room / probe ray the CPU already resolves to the
// prim's physical material (measureRoom), so the bounce rays' hits -- mostly the same surfaces --
// get the prim's real material; anything not listed is classed on the GPU by its flags alone.
// Entries older than kMatAge are not sent (an instance id may be reused).
constexpr U32 kMatSlots = 256;
constexpr F64 kMatAge = 10.0;
struct MatEntry
{
    U32 inst = 0xFFFFFFFFu;
    U8  cls = BC_WOOD;
    F64 at = 0.0;
};
MatEntry sMat[kMatSlots];

U32 matHash(U32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void noteMat(U32 inst, S32 code, F64 now)
{
    if (inst >= 0xFFFFFFFEu || code < 0) return;
    const U8 cls = classOfCode(code);
    const U32 h = matHash(inst) & (kMatSlots - 1);
    MatEntry* slot = nullptr;
    for (U32 k = 0; k < 8; ++k)
    {
        MatEntry& e = sMat[(h + k) & (kMatSlots - 1)];
        if (e.inst == inst) { e.cls = cls; e.at = now; return; }
        if (!slot || e.at < slot->at) slot = &e;   // (empty entries have at 0)
    }
    slot->inst = inst;
    slot->cls = cls;
    slot->at = now;
}

// Add one answer's set to an accumulator, forgetting the old sums with kBounceTau of real time.
void accumulate(BounceAcc& a, const LLVKAcousticsBounceSet& b, U32 bounces, F64 now)
{
    a.bounces = bounces;
    const F32 dt = a.last_at > 0.0 ? llclamp((F32)(now - a.last_at), 0.f, 5.f) : 5.f;
    const F32 k = std::exp(-dt / kBounceTau);   // (the first answer: the old sums vanish)
    a.last_at = now;
    auto mix = [k](F32& x, F32 y) { x = x * k + y; };
    mix(a.rays, (F32)b.rays);
    mix(a.hits, b.hits);
    mix(a.escapes, b.escapes);
    mix(a.capped, b.capped);
    mix(a.nee, b.nee_vis);
    mix(a.spec, b.spec);
    mix(a.seg_len, b.seg_len);
    for (U32 bd = 0; bd < 3; ++bd)
        for (U32 i = 0; i < LLVKAcousticsBounceSet::kBins; ++i) mix(a.hist[bd][i], b.hist[bd][i]);
    for (U32 i = 0; i < LLVKAcousticsBounceSet::kEarlyBins; ++i) mix(a.early[i], b.early[i]);
    for (U32 i = 0; i < LLVKAcousticsBounceSet::kLateBins; ++i) mix(a.late[i], b.late[i]);
    for (U32 i = 0; i < LLVKAcousticsBounceSet::kCapBins; ++i) mix(a.cap[i], b.cap[i]);
    for (U32 i = 0; i < 3; ++i)
    {
        mix(a.edir[i], b.early_dir[i]);
        mix(a.ldir[i], b.late_dir[i]);
        mix(a.e_pre[i], b.e_pre[i]);
        mix(a.e_abs[i], b.e_abs[i]);
        mix(a.e_esc[i], b.e_esc[i]);
        mix(a.e_air[i], b.e_air[i]);
    }
}

// The time (s) by which `frac` of a histogram's energy has arrived (mid of that bin); -1: empty.
F32 onsetTime(const F32* h, U32 n, F32 bin_s, F32 frac)
{
    F32 tot = 0.f;
    for (U32 i = 0; i < n; ++i) tot += h[i];
    if (!(tot > 1e-9f)) return -1.f;
    F32 cum = 0.f;
    for (U32 i = 0; i < n; ++i)
    {
        cum += h[i];
        if (cum >= frac * tot) return ((F32)i + 0.5f) * bin_s;
    }
    return ((F32)n - 0.5f) * bin_s;
}

// The accumulator -> the measurement (see "MEASURED REVERB" at the top).
void analyseBounce(const BounceAcc& a, Measured& m, F64 now)
{
    using BS = LLVKAcousticsBounceSet;
    m.at = now;
    const F32 N = a.rays;
    if (!(N >= 32.f))
    {
        m.valid = false;
        m.conf = m.conf_decay = 0.f;
        return;
    }
    m.valid = true;
    m.rays = N;
    m.hits_per_ray = a.hits / N;
    m.esc_frac = a.escapes / N;
    m.cap_frac = a.capped / N;
    m.nee_per_ray = a.nee / N;
    m.spec_per_ray = a.spec / N;
    const F32 tbar = a.hits > 0.5f ? a.seg_len / a.hits / kSpeed : 0.f;   // mean free time (s)
    m.mfp = tbar * kSpeed;

    // The echogram's WINDOW: only the part the bounce cap has not yet cut into. Rays that escape
    // are physics (their energy left the space); rays the cap ends are not, and they end together
    // (after B mean free paths), so beyond the time the first 10 % of them are gone the echogram
    // falls off for no physical reason. (When the cap ends under 5 % of all rays it never
    // matters.) Inside the window the few capped ones are made up for (survival correction).
    constexpr U32 nb = BS::kBins;
    const F32 dt = BS::kBinS;
    F32 cap_cum[BS::kCapBins];
    F32 cap_tot = 0.f;
    for (U32 j = 0; j < BS::kCapBins; ++j) { cap_tot += a.cap[j]; cap_cum[j] = cap_tot; }
    const F32 cap_cut = std::max(0.1f * cap_tot, 0.05f * N);
    F32 surv[nb];
    std::fill(surv, surv + nb, 1.f);
    U32 n_end = nb;
    for (U32 i = 0; i < nb; ++i)
    {
        const U32 j = std::min<U32>((U32)(((F32)i + 0.5f) * dt / BS::kCapBinS), BS::kCapBins - 1);
        if (cap_cum[j] > cap_cut) { n_end = i; break; }
        surv[i] = 1.f - cap_cum[j] / N;
    }

    F32 etot[3] = {0.f, 0.f, 0.f}, eini[3] = {0.f, 0.f, 0.f};
    F32 retain_mid = 0.f;   // energy kept per reflection (mid; open area, surfaces, air over one free path)
    for (U32 b = 0; b < 3; ++b)
    {
        // RATE: the population's measured loss rate (energy, 1/s).
        F32 lam = airPerM(b) * kSpeed;
        const F32 pre = a.e_pre[b], ab = a.e_abs[b], esc = a.e_esc[b];
        if (tbar > 1e-4f && pre + esc > 1e-6f)
        {
            const F32 alpha = pre > 1e-9f ? llclamp(ab / pre, 0.f, 1.f) : 0.f;
            const F32 p = llclamp(esc / (esc + pre), 0.f, 1.f);
            lam += -std::log(std::max((1.f - p) * (1.f - alpha), 1e-6f)) / tbar;
            if (b == 1) retain_mid = (1.f - p) * (1.f - alpha) * std::exp(-airPerM(b) * m.mfp);
        }
        else
            lam += 200.f;   // (nothing hit: nothing rings)
        m.t_rate[b] = llclamp(13.8155f / lam, 0.05f, 30.f);

        // FIT: the survival-corrected echogram, its tail at the rate, Schroeder, a line in dB.
        F32 h[nb];
        F32 sum = 0.f;
        const U32 n = n_end;
        for (U32 i = 0; i < n; ++i)
        {
            h[i] = a.hist[b][i] / N / std::max(surv[i], 0.5f);
            sum += h[i];
            if ((F32)i * dt < 0.08f) eini[b] += h[i];
        }
        F32 tail = 0.f;
        if (n >= 4)
        {
            F32 hend = 0.f;
            for (U32 i = n - 4; i < n; ++i) hend += h[i];
            tail = (0.25f * hend / dt) / lam;
        }
        etot[b] = sum + tail;
        m.t_fit[b] = m.fit_r2[b] = m.fit_span[b] = m.fit_w[b] = m.covered[b] = 0.f;
        if (n >= 8 && etot[b] > 1e-12f)
        {
            F32 edc[nb];
            F32 acc = tail;
            for (U32 i = n; i-- > 0;) { acc += h[i]; edc[i] = acc; }
            const F32 e0 = edc[0];
            const F32 covered = tail > 0.f ? -10.f * std::log10(std::max(tail / e0, 1e-12f)) : 60.f;
            m.covered[b] = covered;
            const F32 start = covered >= 12.f ? -5.f : -1.f;
            const F32 end = std::max(covered >= 40.f ? -35.f : -25.f, -(covered - 1.f));
            if (start - end >= 3.f)
            {
                F64 sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
                U32 k = 0;
                for (U32 i = 0; i < n; ++i)
                {
                    const F32 db = 10.f * std::log10(std::max(edc[i] / e0, 1e-12f));
                    if (db > start) continue;
                    if (db < end) break;
                    const F64 x = ((F64)i + 0.5) * dt;
                    sx += x; sy += db; sxx += x * x; sxy += x * db; syy += (F64)db * db;
                    ++k;
                }
                if (k >= 4)
                {
                    const F64 vx = sxx - sx * sx / k, vy = syy - sy * sy / k, cxy = sxy - sx * sy / k;
                    if (vx > 1e-12 && vy > 1e-12 && cxy < 0.0)
                    {
                        const F64 slope = cxy / vx;   // dB / s
                        m.t_fit[b] = llclamp((F32)(-60.0 / slope), 0.05f, 30.f);
                        m.fit_r2[b] = (F32)(cxy * cxy / (vx * vy));
                        m.fit_span[b] = start - end;
                        m.fit_w[b] = smooth01(3.f, 25.f, start - end) * smooth01(0.85f, 0.98f, m.fit_r2[b]);
                    }
                }
            }
        }
        m.t60[b] = m.fit_w[b] > 0.f ? std::exp(lerpf(std::log(m.t_rate[b]), std::log(m.t_fit[b]), m.fit_w[b]))
                                    : m.t_rate[b];
    }

    // Containment: how much the GEOMETRY keeps the sound in (a closed room 1, open ground ~0.3):
    // the share of the ray segments that end on a surface rather than escaping. #930: it used to be
    // energy-weighted, 1 - escaped / (escaped + absorbed + air) -- which counts energy the surfaces
    // ABSORB as "kept": on open ground under deep snow the first ground hit absorbs ~85 % of every
    // ray (that energy no longer escapes), so open snowy ground read as a nearly closed space and
    // the measured wet went UP with snow. Counts are absorption-independent (the rays carry on after
    // a hit with what is left); the absorption itself enters through the room constant (reverbFromRoom).
    {
        const F32 pre = a.e_pre[1], ab = a.e_abs[1];
        const F32 segs = a.hits + a.escapes;
        m.containment = segs > 0.5f ? llclamp(a.hits / segs, 0.f, 1.f) : 0.f;
        const F32 alpha = pre > 1e-9f ? ab / pre : 0.f;
        m.absorbent = smooth01(0.06f, 0.25f, alpha - kBounceUnmodelled);
        m.alpha_mid = alpha;
    }
    // Early reflections / late field.
    F32 ee = 0.f, le = 0.f;
    for (U32 i = 0; i < BS::kEarlyBins; ++i) ee += a.early[i];
    for (U32 i = 0; i < BS::kLateBins; ++i) le += a.late[i];
    ee /= N;
    le /= N;
    m.e_early = ee;
    // The LATE energy: orders 3..B as traced, completed past the cap by the geometric series at
    // the measured retention per reflection r (orders 3..B are 1 - r^(B-2) of all orders >= 3).
    const F32 r = llclamp(retain_mid, 0.f, 0.995f);
    const F32 seen = 1.f - std::pow(r, (F32)std::max<S32>((S32)a.bounces - 2, 1));
    m.e_late = le / std::max(seen, 0.05f);
    m.refl_gain = llclamp(kReflCal * 1.26f * std::sqrt(ee / std::max(m.e_late, 1e-9f)), 0.03f, 1.5f);
    const F32 t_e = onsetTime(a.early, BS::kEarlyBins, BS::kEarlyBinS, 0.1f);
    F32 t_l = onsetTime(a.late, BS::kLateBins, BS::kLateBinS, 0.1f);
    // (In a corner the corner's own third-order reflections come first; the diffuse tail does not
    // start before ~2.5 mean free paths.)
    if (t_l >= 0.f) t_l = std::max(t_l, 2.5f * tbar);
    m.refl_delay = t_e >= 0.f ? llclamp(t_e, 0.001f, 0.3f) : 0.02f;
    m.late_delay = t_l >= 0.f ? llclamp(t_l - m.refl_delay, 0.001f, 0.1f) : 0.02f;
    {
        const LLVector3 ed(a.edir[0], a.edir[1], a.edir[2]);
        const F32 el = ed.length();
        m.early_conc = ee > 1e-9f ? llclamp(el / N / ee, 0.f, 1.f) : 0.f;
        m.early_dir = el > 1e-9f ? ed / el : LLVector3::zero;
        const LLVector3 ld(a.ldir[0], a.ldir[1], a.ldir[2]);
        const F32 ll = ld.length();
        m.late_conc = le > 1e-9f ? llclamp(ll / N / le, 0.f, 1.f) : 0.f;
        m.late_dir = ll > 1e-9f ? ld / ll : LLVector3::zero;
    }
    // The reverb's initial spectrum (first 80 ms) against mid.
    m.gain_hf = eini[1] > 1e-9f ? llclamp(std::sqrt(eini[2] / eini[1]), 0.1f, 1.f) : 0.89f;
    m.gain_lf = eini[1] > 1e-9f ? llclamp(std::sqrt(eini[0] / eini[1]), 0.1f, 1.f) : 1.f;
    m.conf = smooth01(200.f, 1200.f, N);
    m.conf_decay = m.conf * smooth01(30.f, 300.f, a.hits);
}

// #897: how hard a surface is for the boundary bass (0 soft .. 1 rigid), from the room rules.
// (#953 water: seen from above, acoustically rigid; seen from below -- the ear in the water -- a
// pressure-release surface: no pressure doubling there, so no boost. dir_z: the ray's.)
F32 bndHardness(S32 code, bool fol, bool gl, F32 hint, F32 dir_z = -1.f)
{
    if (waterHint(hint)) return dir_z < 0.f ? 0.95f : 0.f;
    if (hint < 0.f) return 0.7f;   // terrain
    if (fol) return 0.2f;
    F32 h = gl ? 0.65f : 0.75f;    // a glass pane / the wood default
    switch (code)
    {
    case LL_MCODE_STONE:   h = 1.f; break;
    case LL_MCODE_METAL:   h = 0.95f; break;
    case LL_MCODE_GLASS:   h = 0.65f; break;
    case LL_MCODE_PLASTIC: h = 0.85f; break;
    case LL_MCODE_FLESH:   h = 0.4f; break;
    case LL_MCODE_RUBBER:  h = 0.6f; break;
    default: break;
    }
    return lerpf(h, 1.f, llclamp(hint, 0.f, 1.f));
}

// #897: smooth one answer's boundary sample in (dt: real seconds since the last one).
void updateBoundary(Boundary& B, const RoomSample& x, F32 dt)
{
    for (U32 a = 0; a < 6; ++a)
    {
        if (x.bnd_d[a] < 0.f) continue;   // (no ray in that cone this time)
        if (!B.valid)
        {
            B.d[a] = x.bnd_d[a];
            B.h[a] = x.bnd_h[a];
            continue;
        }
        B.d[a] = lerpf(B.d[a], x.bnd_d[a], follow(x.bnd_d[a] < B.d[a] ? kBndAttack : kBndRelease, dt));
        B.h[a] = lerpf(B.h[a], x.bnd_h[a], follow(kBndRelease, dt));
    }
    B.valid = true;
}

// #897: the boost one end's surroundings give (dB), and how many boundaries that is.
F32 boundaryDb(const Boundary& B, F32* count = nullptr)
{
    F32 n = 0.f;
    if (B.valid)
        for (U32 a = 0; a < 6; ++a) n += B.h[a] * (1.f - smooth01(0.5f * kBassQuarter, 1.5f * kBassQuarter, B.d[a]));
    if (count) *count = n;
    return std::min(kBassPerBoundary * n, kBassMaxPerEnd);
}

// How much a measurement still counts (fading out kMeasStaleLo .. kMeasStaleHi after its answer).
F32 measAge(const Measured& m, F64 now)
{
    return m.valid ? 1.f - smooth01((F32)kMeasStaleLo, (F32)kMeasStaleHi, (F32)(now - m.at)) : 0.f;
}

// #683 early gain: the receiver radius of a source's early-gain set at distance D (m): specular
// returns get likelier at range (fewer, noisier detections at 128 rays otherwise), never
// overlapping the source. The free-field reference uses the same sphere, so the ratio does not
// depend on it.
F32 earlyRecvRadius(F32 D) { return std::min(llclamp(0.15f * D, 0.5f, 2.f), 0.5f * D); }

// #683 early gain: the arrivals within [0, t_end] s of one band of a set's echogram (the last
// bin pro rata).
F32 windowEnergy(const F32* h, F32 t_end)
{
    using BS = LLVKAcousticsBounceSet;
    if (!(t_end > 0.f)) return 0.f;
    const F32 x = t_end / BS::kBinS;
    const U32 full = (U32)std::min(x, (F32)BS::kBins);
    F32 e = 0.f;
    for (U32 i = 0; i < full; ++i) e += h[i];
    if (full < BS::kBins) e += h[full] * (x - (F32)full);
    return e;
}

// #683 early gain SPATIAL CACHE: the newest measurements by emitter AND listener position, so a
// sound that starts where something was measured recently (a gesture one-shot -- every trigger is
// a new audio source id --, the next clip of a chain, a new voice of the same prim) gets its early
// gain at once instead of after the pick / measure / ramp (~0.3-0.5 s, often longer than the
// sound). Fixed size (kEgCacheSlots, the oldest gives way), entries older than kEgCacheAge never
// match; a match needs both ends within kEgCacheMatch of the entry's (the geometry between them is
// what was measured). Same forgetting as the sources' own accumulators.
constexpr U32 kEgCacheSlots = 64;
constexpr F32 kEgCacheMatch = 1.5f;      // m: emitter and listener each this close to the entry's
constexpr F64 kEgCacheAge = 20.0;        // s: an entry this old no longer seeds anything
struct EgCacheEntry
{
    LLVector3 S, L;
    F32 num[3] = {0.f, 0.f, 0.f};
    F32 den = 0.f;
    F64 at = 0.0;                        // 0 = empty
};
EgCacheEntry sEgCache[kEgCacheSlots];

// The best live entry for emitter S / listener L (nullptr: none). create: else the slot to reuse
// (empty or oldest), reset to (S, L).
EgCacheEntry* egCacheFind(const LLVector3& S, const LLVector3& L, F64 now, bool create)
{
    EgCacheEntry* best = nullptr;
    F32 best_d = 1e30f;
    for (EgCacheEntry& e : sEgCache)
    {
        if (e.at <= 0.0 || now - e.at > kEgCacheAge) continue;
        const F32 ds = dist_vec(S, e.S), dl = dist_vec(L, e.L);
        if (ds > kEgCacheMatch || dl > kEgCacheMatch) continue;
        if (ds + dl < best_d) { best_d = ds + dl; best = &e; }
    }
    if (best || !create) return best;
    EgCacheEntry* slot = &sEgCache[0];
    for (EgCacheEntry& e : sEgCache)
    {
        if (e.at <= 0.0) { slot = &e; break; }
        if (e.at < slot->at) slot = &e;
    }
    *slot = EgCacheEntry{};
    slot->S = S;
    slot->L = L;
    return slot;
}

void egCacheClear()
{
    for (EgCacheEntry& e : sEgCache) e = EgCacheEntry{};
}

// #683 early gain: add one early-gain set's answer to a forgotten accumulator (num[3] / den / at).
void accumulateEarlyInto(F32 num[3], F32& den, F64& at, const LLVKAcousticsBounceSet& b, F32 window, F64 now)
{
    const F32 dt = at > 0.0 ? llclamp((F32)(now - at), 0.f, 5.f) : 5.f;
    const F32 k = std::exp(-dt / kEarlyAccTau);
    at = now;
    const LLVector3 S(b.origin[0], b.origin[1], b.origin[2]);
    const LLVector3 R(b.recv[0], b.recv[1], b.recv[2]);
    const F32 D = dist_vec(S, R);
    const F32 rr = std::max(b.recv_r, 0.05f);
    const bool ok = b.rays > 0 && D >= 0.5f * kEarlyMinDist;
    den = den * k + (ok ? (F32)b.rays : 0.f);
    const F32 t_end = D / kSpeed + llclamp(window, 0.f, 0.5f);
    for (U32 band = 0; band < 3; ++band)
    {
        F32 add = 0.f;
        if (ok)
        {
            const F32 efree = rr * rr / (4.f * D * D) * std::exp(-airPerM(band) * D);   // per ray
            add = windowEnergy(b.hist[band], t_end) / std::max(efree, 1e-9f);
        }
        num[band] = num[band] * k + add;
    }
}

// #683 early gain: add one early-gain set's answer to its source (see "EARLY GAIN") and to the
// spatial cache entry of its emitter / listener positions.
void accumulateEarly(SrcState& t, const LLVKAcousticsBounceSet& b, F32 window, F64 now)
{
    accumulateEarlyInto(t.eg_num, t.eg_den, t.eg_acc_at, b, window, now);
    t.eg_rays_win += (F32)b.rays;
    t.eg_hits_win += b.hits;
    t.eg_nee_win += b.nee_vis;
    t.eg_spec_win += b.spec;
    t.eg_rr = b.recv_r;
    t.eg_window = window;
    const LLVector3 S(b.origin[0], b.origin[1], b.origin[2]);
    const LLVector3 R(b.recv[0], b.recv[1], b.recv[2]);
    if (EgCacheEntry* e = egCacheFind(S, R, now, true))
    {
        accumulateEarlyInto(e->num, e->den, e->at, b, window, now);
        e->S = S;
        e->L = R;
    }
}

// The OpenAL ear (audio_update_listener's MediaSoundsEarLocation rule).
LLVector3d earGlobal()
{
    static LLCachedControl<S32> s_ear(gSavedSettings, "MediaSoundsEarLocation", 0);
    return s_ear == 1 ? gAgent.getPositionGlobal() : gAgentCamera.getCameraPositionGlobal();
}

// The physical material code (LL_MCODE_*) of the prim behind clean Instance `inst`, or -1.
S32 instMaterial(LLCleanInstanceStore& store, U32 inst)
{
    if (inst == 0xFFFFFFFFu) return -1;
    LLUUID oid;
    S32 face = 0;
    if (!store.keyForId(inst, oid, face)) return -1;
    const LLViewerObject* obj = gObjectList.findObject(oid);
    return obj ? (S32)(obj->getMaterial() & LL_MCODE_MASK) : -1;
}

// An alpha-mask prim with an explicitly HARD physical material is a solid screen, not foliage.
bool hardMaterial(S32 code)
{
    return code == LL_MCODE_STONE || code == LL_MCODE_METAL || code == LL_MCODE_GLASS || code == LL_MCODE_PLASTIC;
}

// Occlusion weight of a visible alpha-blend surface: glass pane unless the material says wall.
F32 blendOccWeight(S32 code)
{
    return (code == LL_MCODE_STONE || code == LL_MCODE_METAL) ? 1.f : kOccGlass;
}

// Occlusion weight of a rendered alpha-mask surface: foliage unless the material says hard.
F32 maskOccWeight(S32 code)
{
    if (code == LL_MCODE_STONE || code == LL_MCODE_METAL) return 1.f;
    if (code == LL_MCODE_GLASS || code == LL_MCODE_PLASTIC) return kOccGlass;
    return kOccFoliage;
}

void resetChannels()
{
    if (!gAudiop) return;
    for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
    {
        if (LLAudioChannel* ch = gAudiop->getChannelAt(i))
        {
            ch->setAcousticsDirect(1.f, 1.f);
            ch->setAcousticsSend(1.f, 1.f);
            ch->setAcousticsZone(LLUUID::null, -1, 0.f, 1.f);
            ch->setAcousticsPosition(LLUUID::null, false, LLVector3d());
            ch->setAcousticsBass(LLUUID::null, 0.f);
            ch->setAcousticsBoost(LLUUID::null, 1.f);
        }
    }
}

// #897: per world axis the nearest surface straight out of the ray origin (see BOUNDARY BASS).
void measureBoundary(const RaySet& r, const S32* code, const bool* foliage, const bool* glass, RoomSample& out)
{
    for (U32 a = 0; a < 6; ++a)
    {
        out.bnd_d[a] = -1.f;
        out.bnd_h[a] = 0.f;
    }
    for (U32 i = 0; i < r.n; ++i)
    {
        const F32 comp[3] = {r.dir_x[i], r.dir_y[i], r.dir_z[i]};
        F32 hard = r.dist[i] < 0.f ? 0.f : bndHardness(code[i], foliage[i], glass[i], r.hint[i], r.dir_z[i]);
        if (r.dist[i] >= 0.f && r.snow[i] > 0.f) hard = lerpf(hard, snowBndHard(r.snow_pack[i]), r.snow[i]);   // #930
        for (U32 ax = 0; ax < 3; ++ax)
        {
            const F32 c = comp[ax];
            if (std::fabs(c) < kBndCone) continue;
            const U32 slot = ax * 2 + (c > 0.f ? 1 : 0);
            const F32 d = r.dist[i] < 0.f ? kBndFar : r.dist[i] * std::fabs(c);
            if (out.bnd_d[slot] < 0.f || d < out.bnd_d[slot])
            {
                out.bnd_d[slot] = d;
                out.bnd_h[slot] = hard;
            }
        }
    }
}

// The instantaneous room of one ray set (the listener model; counters for the listener only).
void measureRoom(const RaySet& r, HitCounts* hc, RoomSample& out)
{
    const U32 kN = r.n;
    // Per ray: the prim's physical material (once) and its class. kind 1 = alpha-mask
    // (foliage unless alpha blocks + a hard material: a solid screen), 2 = visible
    // alpha-blend (glass class).
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    S32  code[kRoomRays];
    bool foliage[kRoomRays], glass[kRoomRays];
    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (U32 i = 0; i < kN; ++i)
    {
        code[i] = r.dist[i] < 0.f ? -1 : instMaterial(store, r.inst[i]);
        foliage[i] = r.kind[i] == 1 && !(sAlphaBlocks && hardMaterial(code[i]));
        glass[i] = r.kind[i] == 2;
        noteMat(r.inst[i], code[i], now);   // #683 phase 1: the bounce rays' material table
    }
    // Floor rays (z < -0.3) are left out of the enclosure: the ground is under you
    // everywhere, it says nothing about walls. Enclosure = (wall-weighted) hit fraction of
    // the rest; roof = of the upward ones. Foliage counts kFoliageWall of a wall.
    U32 n = 0, n_up = 0;
    F32 hit = 0.f, hit_up = 0.f, dsum = 0.f;
    for (U32 i = 0; i < kN; ++i)
    {
        const F32 z = r.dir_z[i];
        const F32 d = r.dist[i];
        if (z < -0.3f) continue;
        ++n;
        const F32 w = d < 0.f ? 0.f : (foliage[i] ? kFoliageWall : 1.f);
        hit += w;
        dsum += w * d;
        if (z > 0.5f)
        {
            ++n_up;
            hit_up += w;
        }
    }
    out = RoomSample{};
    if (n == 0) return;

    // What the roof is made of (#783 weather audio): the solid upward hits by class. Glass =
    // visible alpha-blend or the glass code; metal = the metal code or a hard render hint
    // (as the absorption below); tent = plastic / rubber (SL has no fabric code); wood = the
    // SL default, and stone too (no concrete-roof bed: a dull wood is the nearest). (Terrain
    // and #953 water -- the surface above an ear in the water -- are no roof: hint < 0.)
    for (U32 i = 0; i < kN; ++i)
    {
        if (r.dir_z[i] <= 0.5f || r.dist[i] < 0.f || foliage[i] || r.hint[i] < 0.f) continue;
        U32 m = LLCleanAcoustics::ROOF_WOOD;
        if (glass[i] || code[i] == LL_MCODE_GLASS) m = LLCleanAcoustics::ROOF_GLASS;
        else if (code[i] == LL_MCODE_METAL || (code[i] != LL_MCODE_STONE && r.hint[i] > 0.5f)) m = LLCleanAcoustics::ROOF_METAL;
        else if (code[i] == LL_MCODE_PLASTIC || code[i] == LL_MCODE_RUBBER) m = LLCleanAcoustics::ROOF_TENT;
        out.mat[m] += 1.f;
        out.mat_n += 1.f;
    }

    // Surface absorption of the hits (every hit, floor included: a carpet counts), the
    // escape fraction of ALL rays (open area: it feeds the absorption, not the size), and
    // geometric clutter (solid hits only: leaf cut-outs are noise, not furniture).
    F32 am = 0.f, ah = 0.f, csum = 0.f;
    U32 na = 0, nc = 0, miss = 0;
    for (U32 i = 0; i < kN; ++i)
    {
        if (r.dist[i] < 0.f) { ++miss; continue; }
        const bool fol = foliage[i];
        if (!fol && r.clutter[i] >= 0.f) { csum += r.clutter[i]; ++nc; }
        // Glass (visible alpha-blend): hard and reflective unless the material says otherwise.
        Absorb a = fol ? kAbsFoliage : glass[i] ? kAbsGlass : kAbsWood;
        const F32 hint = fol ? 0.f : r.hint[i];
        bool hard = glass[i];
        if (waterHint(hint))
        {
            a = kAbsWater;   // #953 (the bounce pass's water class)
            hard = true;
            if (hc) ++hc->water;
        }
        else if (hint < 0.f)
        {
            a = kAbsTerrain;
            hard = false;
            if (hc) ++hc->terrain;
        }
        else
        {
            if (fol && hc) ++hc->foliage;
            if (glass[i] && hc) ++hc->glass;
            if (code[i] >= 0)
            {
                // (Soft codes on foliage: never livelier than the foliage default. Wood /
                // light on glass: "not set", glass stays.)
                switch (code[i])
                {
                case LL_MCODE_STONE:   a = kAbsStone; hard = true; break;
                case LL_MCODE_METAL:   a = kAbsMetal; hard = true; break;
                case LL_MCODE_GLASS:   a = kAbsGlass; hard = true; break;
                case LL_MCODE_FLESH:   a = {std::max(kAbsFlesh.mid, fol ? a.mid : 0.f), std::max(kAbsFlesh.hf, fol ? a.hf : 0.f)};
                                       hard = false; if (hc) ++hc->soft; break;
                case LL_MCODE_PLASTIC: a = kAbsPlastic; break;
                case LL_MCODE_RUBBER:  a = {std::max(kAbsRubber.mid, fol ? a.mid : 0.f), std::max(kAbsRubber.hf, fol ? a.hf : 0.f)};
                                       hard = false; if (hc) ++hc->soft; break;
                default: break;        // wood (the default), light: wood / foliage as set above
                }
            }
            const F32 h = llclamp(hint, 0.f, 1.f);
            a.mid = lerpf(a.mid, std::min(a.mid, kAbsHard), h);
            a.hf = lerpf(a.hf, std::min(a.hf, kAbsHard), h);
            if ((hard || h > 0.5f) && hc) ++hc->hard;
        }
        // #930: lying snow on this hit (its share already x the strength; 0 without snow).
        if (r.snow[i] > 0.f)
        {
            const SnowAbs sa = snowAbs(r.snow_pack[i]);
            a.mid = lerpf(a.mid, sa.mid, r.snow[i]);
            a.hf = lerpf(a.hf, sa.hf, r.snow[i]);
            if (hc) hc->snow += r.snow[i];
        }
        am += a.mid; ah += a.hf; ++na;
    }
    if (hc)
    {
        hc->hits += na;
        hc->rays += kN;
        hc->escapes += miss;
    }
    out.ok = true;
    out.have_abs = na > 0;
    if (out.have_abs) { out.am = am / (F32)na; out.ah = ah / (F32)na; }
    out.have_clut = nc > 0;
    if (out.have_clut) out.clut = llclamp(csum / (F32)nc / kClutterFull, 0.f, 1.f);
    out.enc = hit / (F32)n;
    out.roof = n_up ? hit_up / (F32)n_up : 0.f;
    out.esc = (F32)miss / (F32)kN;
    // Room size: the mean wall distance only as far as we are enclosed (see kSizeOpen).
    out.hit_size = llclamp(hit > 0.f ? dsum / hit : kSizeOpen, 1.f, sRoomRange);
    out.size = llclamp(lerpf(kSizeOpen, out.hit_size, enclosedWeight(out.enc)), 1.f, sRoomRange);
    measureBoundary(r, code, foliage, glass, out);   // #897
}

// Smooth a room toward a sample (a = follow factor; init = take it as is).
void smoothRoom(Room& R, const RoomSample& x, F32 a, bool init)
{
    if (init)
    {
        R.enclosure = x.enc; R.roof = x.roof; R.room_size = x.size; R.escape = x.esc;
        if (x.have_abs) { R.surf_mid = x.am; R.surf_hf = x.ah; }
        if (x.have_clut) R.clutter = x.clut;
        return;
    }
    if (x.have_abs)   // (no hits: escape 1 decides everything; hold the last surfaces)
    {
        R.surf_mid = lerpf(R.surf_mid, x.am, a);
        R.surf_hf = lerpf(R.surf_hf, x.ah, a);
    }
    if (x.have_clut) R.clutter = lerpf(R.clutter, x.clut, a);
    R.enclosure = lerpf(R.enclosure, x.enc, a);
    R.roof = lerpf(R.roof, x.roof, a);
    R.escape = lerpf(R.escape, x.esc, a);
    R.room_size = std::exp(lerpf(std::log(R.room_size), std::log(x.size), a));   // (log domain: 2 m vs 40 m)
}

void roomFromAnswer(State& s, const LLVKAcousticsResult& r, F32 dt)
{
    RaySet rs;
    rs.n = kRoomRays;
    for (U32 i = 0; i < kRoomRays; ++i)
    {
        rs.dist[i] = r.room_dist[i];
        rs.dir_z[i] = r.room_dir_z[i];
        rs.dir_x[i] = r.room_dir_x[i];
        rs.dir_y[i] = r.room_dir_y[i];
        rs.inst[i] = r.room_inst[i];
        rs.hint[i] = r.room_hint[i];
        rs.clutter[i] = r.room_clutter[i];
        rs.kind[i] = r.room_kind[i];
        rs.snow[i] = r.room_snow[i];
        rs.snow_pack[i] = r.room_snow_pack[i];
    }
    RoomSample x;
    measureRoom(rs, &s.hc, x);
    if (!x.ok) return;
    updateBoundary(s.LBnd, x, dt);   // #897
    if (!s.room_valid)
    {
        smoothRoom(s.L, x, 1.f, true);
        s.hit_size = x.hit_size;
        if (x.mat_n > 0.f)
            for (U32 m = 0; m < LLCleanAcoustics::ROOF_MAT_COUNT; ++m) s.roof_mat[m] = x.mat[m] / x.mat_n;
        s.room_valid = true;
        return;
    }
    const F32 a = follow(kRoomTau, dt);
    if (x.mat_n > 0.f)   // (slower than the room: a share flip should not be heard as a switch)
    {
        const F32 am = follow(2.f * kRoomTau, dt);
        for (U32 m = 0; m < LLCleanAcoustics::ROOF_MAT_COUNT; ++m) s.roof_mat[m] = lerpf(s.roof_mat[m], x.mat[m] / x.mat_n, am);
    }
    smoothRoom(s.L, x, a, false);
    s.hit_size = std::exp(lerpf(std::log(s.hit_size), std::log(x.hit_size), a));
}

// meas / blend (#683 phase 1): a bounce measurement and how much it may count (its age / the
// setting); each value moves toward the measured one by blend x its confidence.
LLAcousticsReverb reverbFromRoom(const Room& R, F32 level, F32 bias, RoomDerived* d,
                                 const Measured* meas = nullptr, F32 blend = 0.f)
{
    LLAcousticsReverb rv;
    // How much of a room we are in: the side / up hit fraction with a roof term (under a
    // bridge: little wall, all roof -> some reverb; open ground -> none).
    const F32 room = smooth01(0.25f, 0.85f, 0.7f * R.enclosure + 0.3f * R.roof);
    const F32 size = llclamp(R.room_size, 1.5f, sRoomRange);
    const F32 big = llclamp((size - 2.f) / 30.f, 0.f, 1.f);   // 0 closet .. 1 hall
    // Effective mean absorption of the HITS: surfaces + clutter (only as far as enclosed),
    // scaled by the owner's bias (AudioAcousticsAbsorptionBias -1 livelier x0.35 .. +1
    // deader x2.8).
    const F32 bmul = std::exp2(1.5f * llclamp(bias, -1.f, 1.f));
    const F32 clut = R.clutter * enclosedWeight(R.enclosure);
    const F32 am = llclamp((R.surf_mid + kClutterMid * clut) * bmul, 0.01f, 0.9f);
    const F32 ah = llclamp((R.surf_hf + kClutterHf * clut) * bmul, 0.01f, 0.9f);
    // Over the whole sphere: escaped rays are open area (absorption 1).
    const F32 esc = llclamp(R.escape, 0.f, 1.f);
    const F32 hit_mid = std::min(am + kUnmodelled, 0.95f);
    const F32 tot_mid = std::min(esc + (1.f - esc) * hit_mid, 0.98f);
    const F32 tot_hf = std::min(esc + (1.f - esc) * std::min(ah + kUnmodelled, 0.95f), 0.98f);
    if (d)
    {
        d->eff_mid = am; d->eff_hf = ah;
        d->tot_mid = tot_mid; d->tot_hf = tot_hf;
    }
    const F32 Am = -std::log(1.f - tot_mid);
    const F32 Ah = -std::log(1.f - tot_hf);
    // 0 hard (tile / concrete / bare) .. 1 absorbent (furnished, soft).
    const F32 absorbent = smooth01(0.06f, 0.25f, am);
    // Reverberant level goes with the room constant (1 - a) / a: the open area's share of
    // it quiets the tail further (ratio to the same surfaces closed: exactly 1 indoors).
    const F32 open_level = ((1.f - tot_mid) / tot_mid) / ((1.f - hit_mid) / hit_mid);
    // #930: + the room constant of the hits (1 up to am ~0.35; deep snow ~0.06 -- see roomConstK).
    const F32 room_k = roomConstK(hit_mid, 0.55f);   // (a0: 0.35 surfaces + kUnmodelled)
    rv.wet = room * llclamp(level, 0.f, 1.f) * lerpf(1.f, 0.75f, absorbent)   // a dead room's tail is quieter
             * llclamp(open_level, 0.f, 1.f) * room_k;
    // (A half-open space rings shorter through its escape fraction in Am.)
    rv.decay_time = llclamp(kDecayK * size / Am, 0.2f, 7.f);
    // HF decay / mid decay = the absorption ratio; big rooms lose highs to the air as well.
    // (#930: heavy snowfall takes a little more of the highs out of the air: subtle.)
    rv.decay_hf_ratio = llclamp(Am / Ah * lerpf(1.f, 0.8f, big) * (1.f - 0.08f * sSnowFall), 0.1f, 2.f);
    rv.density = llclamp(size / 20.f, 0.15f, 1.f);
    rv.diffusion = 1.f;
    rv.gain = 0.32f;
    rv.gain_hf = lerpf(0.89f, 0.63f, absorbent);                      // warm
    rv.reflections_gain = lerpf(0.6f, 0.12f, big) * lerpf(1.f, 0.4f, absorbent);   // close hard walls: strong early reflections
    rv.reflections_delay = llclamp(1.2f * size / 343.f, 0.001f, 0.3f);
    rv.late_gain = 1.26f;
    rv.late_delay = llclamp(0.002f + 0.9f * size / 343.f, 0.001f, 0.1f);
    if (d)
    {
        d->stat_decay = rv.decay_time;
        d->stat_wet = rv.wet;
        d->blend = d->blend_decay = 0.f;
        d->stat_room_k = room_k;
        d->meas_wet = 0.f; d->meas_cont = 0.f; d->meas_room_k = 1.f;
    }
    // #683 phase 1: toward the measured room.
    if (meas && meas->valid && blend > 0.f)
    {
        const F32 bw = llclamp(blend, 0.f, 1.f) * meas->conf;
        const F32 bd = llclamp(blend, 0.f, 1.f) * meas->conf_decay;
        // #930 ROOM CONSTANT of the measured space, with the OPEN AREA as absorption 1 (a window):
        //   a_tot = 1 - (1 - p_open) (1 - a_area)
        // a_area: the surfaces' AREA-weighted absorption -- the room rays' (uniform over the sphere,
        //   as Sabine's mean over the surface: am, + the bounce pass's unmodelled share), at least the
        //   bounce hits' own. The bounce hits' absorption alone is ENERGY-weighted: a ray that hits
        //   snow first loses ~85 % at once, so the surviving energy -- and that average -- is
        //   dominated by the walls it bounces between afterwards (owner log: 54 % snow hits, the
        //   measured room constant only x0.88).
        // p_open: the share of segments that escape (1 - containment), at least the room rays'
        //   escape fraction (a ray that bounces among buildings five times before it leaves the
        //   square counts its hits, not the sky it ends in).
        // A closed room (p 0) with ordinary surfaces (a_area <= 0.45) keeps x1: unchanged.
        const F32 a_area = llclamp(std::max(am + kBounceUnmodelled, meas->alpha_mid), 0.f, 0.99f);
        const F32 p_open = llclamp(std::max(1.f - meas->containment, esc), 0.f, 1.f);
        const F32 k_meas = roomConstK(1.f - (1.f - p_open) * (1.f - a_area), 0.45f);
        const F32 wet_m = llclamp(level, 0.f, 1.f) * smooth01(0.15f, 0.75f, meas->containment)
                        * lerpf(1.f, 0.75f, meas->absorbent) * k_meas;
        if (d)
        {
            d->meas_wet = wet_m; d->meas_cont = meas->containment; d->meas_room_k = k_meas;
            d->meas_open = p_open; d->meas_a_area = a_area; d->meas_a_energy = meas->alpha_mid;
        }
        rv.wet = lerpf(rv.wet, wet_m, bw);
        const F32 tm = llclamp(meas->t60[1], 0.1f, 20.f);
        rv.decay_time = std::exp(lerpf(std::log(rv.decay_time), std::log(tm), bd));
        rv.decay_hf_ratio = lerpf(rv.decay_hf_ratio, llclamp(meas->t60[2] / tm, 0.1f, 2.f), bd);
        rv.decay_lf_ratio = lerpf(rv.decay_lf_ratio, llclamp(meas->t60[0] / tm, 0.1f, 2.f), bd);
        rv.reflections_gain = lerpf(rv.reflections_gain, meas->refl_gain, bd);
        rv.reflections_delay = lerpf(rv.reflections_delay, meas->refl_delay, bd);
        rv.late_delay = lerpf(rv.late_delay, meas->late_delay, bd);
        rv.gain_hf = lerpf(rv.gain_hf, meas->gain_hf, bd);
        rv.gain_lf = lerpf(rv.gain_lf, meas->gain_lf, bd);
        if (d)
        {
            d->blend = bw;
            d->blend_decay = bd;
        }
    }
    return rv;
}

// #953: the reverb heard by an ear in the water (see "WATER"): u_c = how much the space is water
// (the speed of sound: decay and delays ~ 1 / c, a denser echo pattern), u_h = the hearing part
// (duller). Exactly `rv` at 0 / 0.
LLAcousticsReverb underwaterReverb(LLAcousticsReverb rv, F32 u_c, F32 u_h)
{
    if (u_c > 0.f)
    {
        const F32 m = std::exp(llclamp(u_c, 0.f, 1.f) * std::log(kSpeed / kSpeedWater));   // (log domain: x1 .. x0.23)
        rv.decay_time = llclamp(rv.decay_time * m, 0.1f, 20.f);
        rv.reflections_delay = llclamp(rv.reflections_delay * m, 0.f, 0.3f);
        rv.late_delay = llclamp(rv.late_delay * m, 0.f, 0.1f);
        rv.density = lerpf(rv.density, kUwDensity, llclamp(u_c, 0.f, 1.f));
    }
    if (u_h > 0.f) rv.gain_hf *= lerpf(1.f, kUwHearHf, llclamp(u_h, 0.f, 1.f));
    return rv;
}

// Zone clustering features of a room, each 0..1: room-ness, size (log), open area, absorbency.
void roomFeature(const Room& R, F32 f[4])
{
    f[0] = smooth01(0.25f, 0.85f, 0.7f * R.enclosure + 0.3f * R.roof);
    f[1] = llclamp(std::log(std::max(R.room_size, kSizeOpen) / kSizeOpen) / std::log(20.f), 0.f, 1.f);
    f[2] = llclamp(R.escape, 0.f, 1.f);
    f[3] = smooth01(0.06f, 0.25f, R.surf_mid + kClutterMid * R.clutter * enclosedWeight(R.enclosure));
}

F32 featureDist(const F32 a[4], const F32 b[4])
{
    F32 d = 0.f;
    for (U32 i = 0; i < 4; ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

// ---- #858 path cache helpers ----

U32 rngNext(State& s)
{
    U32 x = s.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s.rng = x ? x : 0x9E3779B9u;
    return s.rng;
}

F32 rngF(State& s) { return (F32)(rngNext(s) >> 8) * (1.f / 16777216.f); }

// A direction uniformly inside a cone of half-angle `cone` (rad) around unit `axis`.
LLVector3 coneDir(State& s, const LLVector3& axis, F32 cone)
{
    const F32 cz = 1.f - rngF(s) * (1.f - std::cos(cone));
    const F32 sz = std::sqrt(std::max(0.f, 1.f - cz * cz));
    const F32 phi = F_TWO_PI * rngF(s);
    const LLVector3 a = std::fabs(axis.mV[VZ]) < 0.9f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f);
    LLVector3 u = a % axis;
    u.normalize();
    const LLVector3 v = axis % u;
    return axis * cz + u * (sz * std::cos(phi)) + v * (sz * std::sin(phi));
}

// A path's detour weight (bounce losses included), HF retention (turn angles) and detour
// ratio at the current ends. false: degenerate.
bool pathWeight(const Path& p, const LLVector3& L, const LLVector3& S, F32& w, F32& hf, F32& rho)
{
    LLVector3 pts[4];
    U32 n = 0;
    pts[n++] = L;
    pts[n++] = p.p1;
    if (p.bounces == 2) pts[n++] = p.p2;
    pts[n++] = S;
    F32 len = 0.f;
    hf = 1.f;
    for (U32 i = 0; i + 1 < n; ++i) len += dist_vec(pts[i], pts[i + 1]);
    for (U32 i = 1; i + 1 < n; ++i)
    {
        const LLVector3 a = pts[i - 1] - pts[i];
        const LLVector3 b = pts[i + 1] - pts[i];
        const F32 la = a.length(), lb = b.length();
        if (la < 1e-3f || lb < 1e-3f) return false;
        const F32 cosb = llclamp((a * b) / (la * lb), -1.f, 1.f);
        hf *= 1.f - kDiffHf * 0.5f * (1.f + cosb);   // straight through keeps the highs, folding back loses most
    }
    rho = len / std::max(dist_vec(L, S), 0.5f);
    w = std::exp(-std::max(rho - 1.f, 0.f) / kDetourScale) * (p.bounces == 2 ? kBounce2 : 1.f) * p.snow_m;
    hf *= p.snow_h;   // (#930: 1 without snow)
    return true;
}

// A path just found clear on every leg: refresh the kept one it duplicates, else keep it
// (an empty slot, else in place of the weakest when stronger).
// snow / snow_pack (#930): the lying snow at p1 (the probe's hit there; 0 when unknown).
void cachePath(State& s, SrcState& t, U8 bounces, const LLVector3& p1, const LLVector3& p2, const LLVector3& L, F64 now,
               F32 snow = 0.f, F32 snow_pack = 0.f)
{
    F32 sm, sh;
    snowPathLoss(snow, snow_pack, sm, sh);
    for (Path& c : t.paths)
    {
        if (c.uid && c.bounces == bounces && dist_vec(c.p1, p1) < kPathMerge
            && (bounces == 1 || dist_vec(c.p2, p2) < kPathMerge))
        {
            c.last_ok = now;
            c.snow_m = sm; c.snow_h = sh;
            return;
        }
    }
    Path n;
    if (++s.path_uid == 0) ++s.path_uid;
    n.uid = s.path_uid;
    n.bounces = bounces;
    n.p1 = p1;
    n.p2 = p2;
    n.snow_m = sm; n.snow_h = sh;
    n.found = n.last_ok = now;
    F32 w, hf, rho;
    if (!pathWeight(n, L, t.pos, w, hf, rho)) return;
    Path* slot = nullptr;
    F32 worst = 1e9f;
    for (Path& c : t.paths)
    {
        if (!c.uid) { slot = &c; worst = -1.f; break; }
        F32 cw = 0.f, ch, cr;
        if (!pathWeight(c, L, t.pos, cw, ch, cr)) cw = 0.f;
        if (cw < worst) { worst = cw; slot = &c; }
    }
    if (!slot || (slot->uid && worst >= w)) return;   // (full of stronger paths)
    *slot = n;
    ++s.win_found;
    if (bounces == 2) ++s.win_found_2b;
}

// Remember a far point (an opening candidate); near-duplicates refresh, the nearest gives way.
void noteFar(FarPt* arr, U32 n, const LLVector3& p, F32 d, F64 now)
{
    for (U32 i = 0; i < n; ++i)
    {
        FarPt& f = arr[i];
        if (f.at > 0.0 && now - f.at <= kFarAge && dist_vec(f.p, p) < kPathMerge)
        {
            if (d >= f.d) { f.p = p; f.d = d; }
            f.at = now;
            return;
        }
    }
    FarPt* slot = nullptr;
    for (U32 i = 0; i < n; ++i)
    {
        FarPt& f = arr[i];
        if (f.at <= 0.0 || now - f.at > kFarAge) { slot = &f; break; }
        if (!slot || f.d < slot->d) slot = &f;
    }
    if (!slot) return;
    if (slot->at > 0.0 && now - slot->at <= kFarAge && slot->d >= d) return;
    slot->p = p;
    slot->d = d;
    slot->at = now;
}

bool farLive(const FarPt& f, F64 now) { return f.at > 0.0 && now - f.at <= kFarAge; }

U32 livePaths(const SrcState& t, F64 now, bool one_bounce_only = false)
{
    U32 n = 0;
    for (const Path& c : t.paths)
        if (c.uid && now - c.last_ok <= kPathStale && (!one_bounce_only || c.bounces == 1)) ++n;
    return n;
}

// The per-source record for `id` (created; the cap evicts the least recently traced).
SrcState& track(State& s, const LLUUID& id, F64 now)
{
    auto it = s.src.find(id);
    if (it != s.src.end()) return it->second;
    if (s.src.size() >= kMaxTracked)
    {
        auto oldest = s.src.begin();
        for (auto j = s.src.begin(); j != s.src.end(); ++j)
            if (j->second.seen < oldest->second.seen) oldest = j;
        s.src.erase(oldest);
    }
    SrcState& n = s.src[id];
    n.seen = now;
    n.first_seen = now;
    n.room.clutter = s.L.clutter;
    s.src_peak = std::max<U32>(s.src_peak, (U32)s.src.size());
    return n;
}

void occFromAnswer(State& s, const LLVKAcousticsResult& r, F32 dt, F64 now)
{
    const Pending& p = s.ring[r.serial % kRing];
    if (p.serial != r.serial) return;   // its request was overwritten (very late answer)
    const U32 n = std::min(r.source_count, p.n);
    LLCleanInstanceStore& store = LLCleanInstanceStore::instance();
    U32 glass = 0;
    for (U32 k = 0; k < n; ++k)
    {
        // An opaque wall / terrain blocks fully; otherwise the strongest visible alpha surface
        // the ray crossed (alpha blocks: a window pane, a hard screen; foliage kOccFoliage).
        F32 blocked = r.occ_dist[k] >= 0.f ? 1.f : 0.f;
        if (blocked < 1.f && r.occ_blend_inst[k] != 0xFFFFFFFFu)
            blocked = std::max(blocked, blendOccWeight(instMaterial(store, r.occ_blend_inst[k])));
        if (blocked < 1.f && r.occ_mask_inst[k] != 0xFFFFFFFFu)
            blocked = std::max(blocked, maskOccWeight(instMaterial(store, r.occ_mask_inst[k])));
        if (blocked > 0.f && blocked < 1.f) ++glass;
        SrcState& o = track(s, p.ids[k], now);
        if (!o.occ_valid)
        {
            o.occ = blocked;   // first answer: no fade from "clear"
            o.occ_valid = true;
        }
        else
            o.occ = lerpf(o.occ, blocked, follow(blocked > o.occ ? kOccAttack : kOccRelease, dt));
        o.seen = now;
    }
    s.occ_glass = glass;
}

// #857 one source-room probe (queries [i0, i1) of the answer): the source's room and
// coupling; (#858) its end points the listener sees are cached paths, the others far points.
void probeGroup(State& s, SrcState& st, const LLVKAcousticsResult& r, const Pending& p, U32 i0, U32 i1, F64 now)
{
    const U32 si = p.meta[i0].src;
    const LLVector3& S = p.pos[si];
    const LLVector3& L = p.listener;
    RaySet rs;
    rs.n = std::min<U32>(i1 - i0, kRoomRays);
    U32 vis = 0;
    const bool occluded = st.occ_valid && st.occ > 0.2f;
    for (U32 k = 0; k < rs.n; ++k)
    {
        const LLVKAcousticsQueryHit& h = r.query[i0 + k];
        rs.dist[k] = h.dist;
        rs.dir_z[k] = p.meta[i0 + k].dir_z;
        rs.dir_x[k] = p.meta[i0 + k].dir_x;
        rs.dir_y[k] = p.meta[i0 + k].dir_y;
        rs.inst[k] = h.inst;
        rs.hint[k] = h.hint;
        rs.clutter[k] = -1.f;
        rs.kind[k] = h.kind;
        rs.snow[k] = h.snow;
        rs.snow_pack[k] = h.snow_pack;
        const LLVector3 P(h.p[0], h.p[1], h.p[2]);
        if (h.vis >= 0.f)
        {
            ++vis;
            if (occluded) cachePath(s, st, 1, P, P, L, now, h.snow, h.snow_pack);
        }
        else
            noteFar(st.far_s, kSrcFar, P, dist_vec(P, S), now);
    }
    if (rs.n == 0) return;
    s.win_probe_rays += rs.n;
    s.win_probe_vis += vis;
    ++s.win_probes;
    RoomSample x;
    measureRoom(rs, nullptr, x);
    const F32 frac = (F32)vis / (F32)rs.n;
    const F32 dt = st.probe_at > 0.0 ? llclamp((F32)(now - st.probe_at), 0.f, 2.f) : 0.f;
    if (x.ok) updateBoundary(st.bnd, x, dt);   // #897
    if (x.ok)
    {
        if (!st.room_valid)
        {
            st.room.clutter = s.L.clutter;   // (no clutter score from 16 rays: the listener's)
            smoothRoom(st.room, x, 1.f, true);
            st.couple = frac;
            st.room_valid = true;
            st.first_probe_at = now;
        }
        else
        {
            smoothRoom(st.room, x, follow(kSrcRoomTau, dt), false);
            st.couple = lerpf(st.couple, frac, follow(kCoupleTau, dt));
        }
        st.wet = reverbFromRoom(st.room, s.level, s.bias, nullptr, &st.meas, measAge(st.meas, now)).wet;
    }
    st.probe_at = now;
}

// #857 / #858: this answer's path queries (probes, guided discovery, joins, validations).
void queriesFromAnswer(State& s, const LLVKAcousticsResult& r, F64 now)
{
    const Pending& p = s.ring[r.serial % kRing];
    if (p.serial != r.serial) return;
    const U32 nq = std::min(r.query_count, p.nq);
    const LLVector3& L = p.listener;
    struct LegAgg { U32 uid; U16 src; U8 legs, ok, got; };
    LegAgg agg[kMaxQueries];
    U32 na = 0;
    U32 i = 0;
    while (i < nq)
    {
        const QMeta& m = p.meta[i];
        if (m.src >= p.n) { ++i; continue; }
        auto it = s.src.find(p.ids[m.src]);
        SrcState* t = it == s.src.end() ? nullptr : &it->second;
        if (m.type == Q_PROBE)
        {
            U32 j = i;
            while (j < nq && p.meta[j].type == Q_PROBE && p.meta[j].src == m.src && j - i < kProbePer) ++j;
            if (t) probeGroup(s, *t, r, p, i, j, now);
            i = j;
            continue;
        }
        const LLVKAcousticsQueryHit& h = r.query[i];
        const LLVector3 P(h.p[0], h.p[1], h.p[2]);
        switch (m.type)
        {
        case Q_GUIDE_L:   // P is seen by the listener (a ray from it): a path if it sees the source
            if (t)
            {
                if (h.vis >= 0.f) cachePath(s, *t, 1, P, P, L, now, h.snow, h.snow_pack);
                t->disc_at = now;
            }
            break;
        case Q_GUIDE_S:   // P is seen by the source: a path if it sees the listener, else a far point
            if (t)
            {
                if (h.vis >= 0.f) cachePath(s, *t, 1, P, P, L, now, h.snow, h.snow_pack);
                else noteFar(t->far_s, kSrcFar, P, dist_vec(P, p.pos[m.src]), now);
                t->disc_at = now;
            }
            break;
        case Q_JOIN:      // listener far point P1 (= P) -> source far point P2: a two-bounce path
            if (t && h.vis >= 0.f) cachePath(s, *t, 2, P, m.aux, L, now);
            break;
        case Q_LEG:
        {
            U32 a = 0;
            while (a < na && agg[a].uid != m.uid) ++a;
            if (a == na) agg[na++] = LegAgg{m.uid, m.src, m.legs, 0, 0};
            ++agg[a].got;
            if (h.vis >= 0.f) ++agg[a].ok;
            break;
        }
        default: break;
        }
        ++i;
    }
    // Validations: every leg clear -> confirmed; any leg blocked -> dropped now.
    for (U32 a = 0; a < na; ++a)
    {
        auto it = s.src.find(p.ids[agg[a].src]);
        if (it == s.src.end()) continue;
        SrcState& t = it->second;
        t.val_at = now;
        for (Path& c : t.paths)
        {
            if (c.uid != agg[a].uid) continue;
            if (agg[a].got < agg[a].legs) break;   // (incomplete: the budget cut it)
            if (agg[a].ok == agg[a].legs)
            {
                c.last_ok = now;
                ++s.win_valid_ok;
            }
            else
            {
                c.uid = 0;
                ++s.win_valid_lost;
            }
            break;
        }
    }
}

// #858: the room answer's shadow rays (listener room hits -> listed occluded sources): the two
// strongest paths per source go into its cache.
void diffFromAnswer(State& s, const LLVKAcousticsResult& r, F64 now)
{
    const Pending& p = s.ring[r.room_serial % kRing];
    if (p.serial != r.room_serial) return;
    const LLVector3& L = p.listener;
    for (U32 k = 0; k < kMaxDiff; ++k)
    {
        const U32 idx = r.diff_src[k];
        if (idx >= p.n) continue;
        auto it = s.src.find(p.ids[idx]);
        if (it == s.src.end()) continue;
        SrcState& st = it->second;
        F32 bw[2] = {-1.f, -1.f};
        LLVector3 bp[2];
        U32 bi[2] = {0, 0};   // (#930: their room rays, for the snow there)
        bool any = false;
        for (U32 i = 0; i < kRoomRays; ++i)
        {
            if (r.room_dist[i] < 0.f || r.room_nee[i][k] < 0.f) continue;
            LLVector3 dir(r.room_dir_x[i], r.room_dir_y[i], r.room_dir_z[i]);
            dir.normalize();
            const LLVector3 P = L + dir * std::max(r.room_dist[i] - 0.05f, 0.f);   // (the shadow ray's origin)
            Path tmp;
            tmp.p1 = P;
            snowPathLoss(r.room_snow[i], r.room_snow_pack[i], tmp.snow_m, tmp.snow_h);   // #930
            F32 w, hf, rho;
            if (!pathWeight(tmp, L, p.pos[idx], w, hf, rho)) continue;
            any = true;
            if (w > bw[0]) { bw[1] = bw[0]; bp[1] = bp[0]; bi[1] = bi[0]; bw[0] = w; bp[0] = P; bi[0] = i; }
            else if (w > bw[1]) { bw[1] = w; bp[1] = P; bi[1] = i; }
        }
        for (U32 b = 0; b < 2; ++b)
            if (bw[b] >= 0.f) cachePath(s, st, 1, bp[b], bp[b], L, now, r.room_snow[bi[b]], r.room_snow_pack[bi[b]]);
        ++s.win_nee;
        if (any) ++s.win_nee_found;
        st.diff_at = now;
    }
}

// #858: the room answer's far points (hits well beyond the listener's room, and misses):
// where the openings are, for guided rays and two-bounce joins.
void farFromRoom(State& s, const LLVKAcousticsResult& r, F64 now)
{
    const LLVector3 L(r.listener[0], r.listener[1], r.listener[2]);
    const F32 thresh = std::max(1.5f * s.L.room_size, 3.f);
    const F32 miss_reach = std::min(std::max(3.f * s.L.room_size, 6.f), std::max(r.room_range, 1.f));
    for (U32 i = 0; i < kRoomRays; ++i)
    {
        const F32 d = r.room_dist[i];
        if (d >= 0.f && d < thresh) continue;
        LLVector3 dir(r.room_dir_x[i], r.room_dir_y[i], r.room_dir_z[i]);
        dir.normalize();
        if (dir.mV[VZ] < -0.5f) continue;   // (the floor through a gap is not an opening worth chasing)
        const F32 at = d >= 0.f ? std::max(d - 0.1f, 0.f) : miss_reach;
        noteFar(s.lfar, kFarPoints, L + dir * at, d >= 0.f ? d : miss_reach, now);
    }
}

// #858: the kept paths at the current ends -> the smoothed strength and apparent direction.
void evaluatePaths(State& s, SrcState& t, const LLVector3& L, F64 now, F32 dt, F32 diff)
{
    F32 best = 0.f, best_hf = 1.f, best_rho = 0.f, wsum = 0.f;
    U32 n = 0;
    LLVector3 dirsum;
    for (Path& c : t.paths)
    {
        if (!c.uid) continue;
        if (now - c.last_ok > kPathExpire)
        {
            c.uid = 0;
            ++s.win_expired;
            continue;
        }
        if (now - c.last_ok > kPathStale) continue;
        F32 w, hf, rho;
        if (!pathWeight(c, L, t.pos, w, hf, rho)) continue;
        ++n;
        if (w > best) { best = w; best_hf = hf; best_rho = rho; }
        const LLVector3 d = c.p1 - L;
        const F32 l = d.length();
        if (l > 1e-3f)
        {
            dirsum += d * (w / l);
            wsum += w;
        }
    }
    t.n_valid = n;
    t.rho = n ? best_rho : 0.f;
    const F32 qi = n ? diff * kBounce * best * (n >= 2 ? 1.f : 0.8f) : 0.f;
    const F32 qhi = qi * best_hf;
    t.q = lerpf(t.q, qi, follow(qi > t.q ? kQAttack : kQRelease, dt));
    t.q_hf = lerpf(t.q_hf, qhi, follow(qhi > t.q_hf ? kQAttack : kQRelease, dt));
    const F32 len = dirsum.length();
    if (wsum > 0.f && len > 1e-3f)
    {
        const LLVector3 d = dirsum / len;
        const F32 conc = llclamp(len / wsum, 0.f, 1.f);   // (the mean resultant length)
        t.conc = t.ind_dir_valid ? lerpf(t.conc, conc, follow(kIndDirTau, dt)) : conc;
        t.ind_dir = t.ind_dir_valid ? nlerp(t.ind_dir, d, follow(kIndDirTau, dt)) : d;
        t.ind_dir_valid = true;
    }
}

// #857 zones: assign sources wanting a zone (strongest first) with hysteresis, fade sends
// across a retarget, and move each zone's room toward its members'.
void updateZones(State& s, F32 dt, F64 now)
{
    const U32 K = s.zones_avail;
    for (U32 z = 0; z < kMaxZones; ++z)
    {
        s.zones[z].members = 0;
        s.zones[z].fresh = false;
    }
    SrcState* order[kMaxTracked];
    U32 n = 0;
    for (auto& kv : s.src)
        if (n < kMaxTracked) order[n++] = &kv.second;
    std::sort(order, order + n, [](const SrcState* a, const SrcState* b) { return a->zone_want > b->zone_want; });

    F32 wsum[kMaxZones] = {}, enc[kMaxZones] = {}, roof[kMaxZones] = {}, lsize[kMaxZones] = {}, sm[kMaxZones] = {},
        sh[kMaxZones] = {}, clut[kMaxZones] = {}, esc[kMaxZones] = {};
    // #683 phase 1: the members' bounce measurements (weighted by send and confidence).
    struct ZM
    {
        F32 w = 0.f, wc = 0.f, wcd = 0.f, lt[3] = {0.f, 0.f, 0.f}, cont = 0.f, absb = 0.f, rg = 0.f, rd = 0.f, ld = 0.f,
            ghf = 0.f, glf = 0.f, am = 0.f;
    };
    ZM zm[kMaxZones];
    for (U32 i = 0; i < n; ++i)
    {
        SrcState& st = *order[i];
        st.zone_send = lerpf(st.zone_send, st.zone_want, follow(kZoneSendTau, dt));
        S32 desired = -1;
        if (K > 0 && st.zone_want > kZoneMinSend && st.room_valid)
        {
            F32 f[4];
            roomFeature(st.room, f);
            if (st.zone >= 0 && (U32)st.zone < K && s.zones[st.zone].active && featureDist(f, s.zones[st.zone].f) < kZoneStay)
                desired = st.zone;
            else
            {
                S32 best = -1, free_z = -1;
                F32 bd = 1e9f;
                for (U32 z = 0; z < K; ++z)
                {
                    if (!s.zones[z].active) { if (free_z < 0) free_z = (S32)z; continue; }
                    const F32 d = featureDist(f, s.zones[z].f);
                    if (d < bd) { bd = d; best = (S32)z; }
                }
                if (best >= 0 && bd < kZoneJoin) desired = best;
                else if (free_z >= 0)
                {
                    Zone& zn = s.zones[free_z];
                    zn.active = true;
                    zn.fresh = true;
                    zn.room = st.room;
                    std::copy(f, f + 4, zn.f);
                    zn.last_member = now;
                    desired = free_z;
                }
                else desired = best;
            }
        }
        if (st.zone != desired)
        {
            // Ramp the send out before it moves (a retarget at ~0 is silent).
            st.zone_fade = lerpf(st.zone_fade, 0.f, follow(kZoneFadeOut, dt));
            if (st.zone_fade < 0.02f || st.zone < 0 || (U32)st.zone >= K)
            {
                st.zone = desired;
                st.zone_fade = 0.f;
            }
        }
        else
            st.zone_fade = lerpf(st.zone_fade, desired >= 0 ? 1.f : 0.f, follow(kZoneFadeIn, dt));
        if (st.zone >= 0 && (U32)st.zone < K)
        {
            const U32 z = (U32)st.zone;
            Zone& zn = s.zones[z];
            ++zn.members;
            zn.last_member = now;
            const F32 w = st.zone_send * st.zone_fade + 0.01f;
            wsum[z] += w;
            enc[z] += w * st.room.enclosure;
            roof[z] += w * st.room.roof;
            lsize[z] += w * std::log(st.room.room_size);
            sm[z] += w * st.room.surf_mid;
            sh[z] += w * st.room.surf_hf;
            clut[z] += w * st.room.clutter;
            esc[z] += w * st.room.escape;
            const F32 ma = measAge(st.meas, now);
            if (ma > 0.f && st.meas.conf > 0.f)
            {
                ZM& q = zm[z];
                const F32 wm = w * ma * st.meas.conf;
                q.w += wm;
                q.wc += w * ma * st.meas.conf;
                q.wcd += w * ma * st.meas.conf_decay;
                for (U32 b = 0; b < 3; ++b) q.lt[b] += wm * std::log(llclamp(st.meas.t60[b], 0.05f, 30.f));
                q.cont += wm * st.meas.containment;
                q.absb += wm * st.meas.absorbent;
                q.am += wm * st.meas.alpha_mid;
                q.rg += wm * st.meas.refl_gain;
                q.rd += wm * st.meas.refl_delay;
                q.ld += wm * st.meas.late_delay;
                q.ghf += wm * st.meas.gain_hf;
                q.glf += wm * st.meas.gain_lf;
            }
        }
    }
    for (U32 z = 0; z < K; ++z)
    {
        Zone& zn = s.zones[z];
        if (zn.members > 0 && wsum[z] > 0.f && !zn.fresh)
        {
            const F32 a = follow(kZoneRoomTau, dt);
            const F32 iw = 1.f / wsum[z];
            Room& R = zn.room;
            R.enclosure = lerpf(R.enclosure, enc[z] * iw, a);
            R.roof = lerpf(R.roof, roof[z] * iw, a);
            R.room_size = std::exp(lerpf(std::log(R.room_size), lsize[z] * iw, a));
            R.surf_mid = lerpf(R.surf_mid, sm[z] * iw, a);
            R.surf_hf = lerpf(R.surf_hf, sh[z] * iw, a);
            R.clutter = lerpf(R.clutter, clut[z] * iw, a);
            R.escape = lerpf(R.escape, esc[z] * iw, a);
            roomFeature(R, zn.f);
        }
        else if (zn.members == 0 && zn.active && now - zn.last_member > kZoneIdle)
            zn.active = false;   // (its tail rings out; reclaimed with a new room later)
        // #683 phase 1: the members' measurements, confidence relative to all its members' sends.
        const ZM& q = zm[z];
        if (zn.members > 0 && q.w > 1e-6f && wsum[z] > 0.f)
        {
            Measured& M = zn.meas;
            const F32 iq = 1.f / q.w;
            M.valid = true;
            M.at = now;
            M.conf = llclamp(q.wc / wsum[z], 0.f, 1.f);
            M.conf_decay = llclamp(q.wcd / wsum[z], 0.f, 1.f);
            for (U32 b = 0; b < 3; ++b) M.t60[b] = std::exp(q.lt[b] * iq);
            M.containment = q.cont * iq;
            M.absorbent = q.absb * iq;
            M.alpha_mid = q.am * iq;
            M.refl_gain = q.rg * iq;
            M.refl_delay = q.rd * iq;
            M.late_delay = q.ld * iq;
            M.gain_hf = q.ghf * iq;
            M.gain_lf = q.glf * iq;
        }
        else
            zn.meas.valid = false;
    }
}

// ---- #894 OPENINGS AROUND THE LISTENER (weather wind audio) ----
// The listener's room rays (64 per room answer, jittered over the sphere) binned onto kOpenBins
// FIXED directions (a spherical Fibonacci set; the floor bins, z < kOpenFloorZ, are ignored).
// Each ray says how OPEN its direction is: escaped (nothing within the room range: sky, open
// country) = 1; a FAR hit (well beyond the room: through a doorway into a hall, a window onto a
// distant hill -- #858's far points) = kOpenFar x a ramp over [thresh, 2 thresh]; a near hit
// = 0. Each bin keeps an exponentially smoothed open FRACTION (kOpenTau of real time: a bin is
// sampled ~once per answer, the per-frame jitter covers its cone) and the smoothed distance of
// its closed hits (the wall: where an opening in the neighbouring bins sits). Fractions are
// carried, never thresholded per ray: a 1 x 1 m window at 3 m (~0.11 sr) covers about half a
// bin (0.196 sr) and reads as a bin about half open.
// OPENINGS: the bins open more than kOpenBinMin, joined into connected groups over the bins'
// neighbour graph (angle < ~1.4 x the mean spacing); per group the open-weighted direction, the
// solid angle (the open fractions x the bin's 4 pi / kOpenBins) and the distance (the walls of
// the group's closed neighbours; the room size without any). The kMaxOpenings largest are kept.
// Bounded: 64 bins, <= 64 rays per answer, a BFS over 64 nodes; only on new room answers.
// LITE MODE: with AudioAcoustics off (no EFX room model) but ray tracing on, the weather audio
// asks for the openings (wantOpenings, held kOpenWantHold): fillRequest then sends a room-only
// request (no sources, no path queries: 64 rays every AudioAcousticsRoomInterval frames) and
// update harvests just the room rays. Nothing is traced when nobody asked.
constexpr U32   kOpenBins = 64;
constexpr F32   kOpenFloorZ = -0.3f;
constexpr F32   kOpenFar = 0.6f;
constexpr F32   kOpenTau = 2.f;            // s
constexpr F32   kOpenBinMin = 0.25f;
constexpr F32   kOpenNeighbourCos = 0.82f; // ~35 degrees (mean spacing ~25 degrees)
constexpr F64   kOpenStale = 1.5;          // s without a room answer: no openings
constexpr F64   kOpenWantHold = 1.0;       // s a wantOpenings() keeps the lite requests coming
constexpr F32   kOpenBinSr = 4.f * 3.14159265f / (F32)kOpenBins;

struct OpenState
{
    bool      init = false;
    LLVector3 dir[kOpenBins];
    U8        nb[kOpenBins][12];
    U8        nn[kOpenBins] = {};
    F32       open[kOpenBins] = {};
    F32       wall[kOpenBins] = {};
    bool      seen[kOpenBins] = {};
    U32       last_room_serial = 0;
    F64       last_at = 0.0;           // last room answer consumed
    F32       room = 5.f;              // smoothed mean non-floor hit distance (both modes: the far threshold)
    F32       enclosure = 0.f;         // smoothed non-floor hit fraction (lite mode's own; log)
    F32       escape = 0.f;            // smoothed escaped fraction of all rays
    bool      have = false;
    LLCleanAcoustics::Opening out[LLCleanAcoustics::kMaxOpenings];
    U32       n = 0;
    F64       wanted_until = 0.0;
    U32       answers = 0;             // log window
    F64       next_log = 0.0;
};

OpenState& ost()
{
    static OpenState o;
    return o;
}

void openInit(OpenState& o)
{
    if (o.init) return;
    o.init = true;
    for (U32 i = 0; i < kOpenBins; ++i)
    {
        const F32 z = 1.f - 2.f * ((F32)i + 0.5f) / (F32)kOpenBins;
        const F32 phi = 6.28318531f * std::fmod((F32)i * 0.61803398875f, 1.f);
        const F32 r = std::sqrt(std::max(0.f, 1.f - z * z));
        o.dir[i].setVec(r * std::cos(phi), r * std::sin(phi), z);
    }
    for (U32 i = 0; i < kOpenBins; ++i)
    {
        o.nn[i] = 0;
        for (U32 j = 0; j < kOpenBins && o.nn[i] < 12; ++j)
            if (j != i && o.dir[i] * o.dir[j] > kOpenNeighbourCos) o.nb[i][o.nn[i]++] = (U8)j;
    }
}

U32 openNearestBin(const OpenState& o, const LLVector3& d)
{
    U32 best = 0;
    F32 bd = -2.f;
    for (U32 i = 0; i < kOpenBins; ++i)
    {
        const F32 c = o.dir[i] * d;
        if (c > bd) { bd = c; best = i; }
    }
    return best;
}

// One room answer -> the bins, then the openings (both modes; deduplicated on the room serial).
void openingsFromRoom(const LLVKAcousticsResult& r, F64 now)
{
    OpenState& o = ost();
    if (r.room_serial == 0 || r.room_serial == o.last_room_serial) return;
    openInit(o);
    const F32 rdt = o.last_at > 0.0 ? llclamp((F32)(now - o.last_at), 0.f, 2.f) : 0.f;
    o.last_room_serial = r.room_serial;
    o.last_at = now;
    ++o.answers;
    const F32 range = std::max(r.room_range, 1.f);
    // The room's own scale first (the far threshold): the mean non-floor hit, its hit fraction.
    {
        F32 dsum = 0.f, hits = 0.f, n = 0.f, miss = 0.f;
        for (U32 i = 0; i < kRoomRays; ++i)
        {
            if (r.room_dist[i] < 0.f) miss += 1.f;
            if (r.room_dir_z[i] < kOpenFloorZ) continue;
            n += 1.f;
            if (r.room_dist[i] >= 0.f) { dsum += r.room_dist[i]; hits += 1.f; }
        }
        const F32 a = o.have ? follow(kOpenTau, rdt) : 1.f;
        if (hits > 0.f) o.room = std::exp(lerpf(std::log(std::max(o.room, 0.5f)), std::log(std::max(dsum / hits, 0.5f)), a));
        if (n > 0.f) o.enclosure = lerpf(o.enclosure, hits / n, a);
        o.escape = lerpf(o.escape, miss / (F32)kRoomRays, a);
    }
    const F32 thresh = std::max(1.5f * o.room, 3.f);
    // This answer's rays per bin.
    F32 sum[kOpenBins] = {}, cnt[kOpenBins] = {}, wsum[kOpenBins] = {}, wcnt[kOpenBins] = {};
    for (U32 i = 0; i < kRoomRays; ++i)
    {
        LLVector3 d(r.room_dir_x[i], r.room_dir_y[i], r.room_dir_z[i]);
        if (d.normalize() < 1e-3f || d.mV[VZ] < kOpenFloorZ) continue;
        const U32 b = openNearestBin(o, d);
        const F32 dist = r.room_dist[i];
        F32 v = 1.f;   // escaped
        if (dist >= 0.f)
        {
            const F32 t = llclamp((dist - thresh) / std::max(thresh, 1e-3f), 0.f, 1.f);
            v = kOpenFar * t * t * (3.f - 2.f * t);
            if (dist < thresh) { wsum[b] += dist; wcnt[b] += 1.f; }
        }
        sum[b] += v;
        cnt[b] += 1.f;
    }
    for (U32 b = 0; b < kOpenBins; ++b)
    {
        if (cnt[b] <= 0.f) continue;
        const F32 a = (o.have && o.seen[b]) ? follow(kOpenTau, rdt * cnt[b]) : 1.f;
        o.open[b] = lerpf(o.open[b], sum[b] / cnt[b], a);
        if (wcnt[b] > 0.f) o.wall[b] = o.seen[b] && o.wall[b] > 0.f ? lerpf(o.wall[b], wsum[b] / wcnt[b], a) : wsum[b] / wcnt[b];
        o.seen[b] = true;
    }
    o.have = true;
    // Connected groups of open bins.
    U8 group[kOpenBins];
    for (U8& g : group) g = 0xFF;
    struct G { LLVector3 d; F32 sr = 0.f, wd = 0.f, wn = 0.f; };
    G gs[kOpenBins];
    U32 ng = 0;
    U8 queue[kOpenBins];
    for (U32 b = 0; b < kOpenBins; ++b)
    {
        if (group[b] != 0xFF || !o.seen[b] || o.dir[b].mV[VZ] < kOpenFloorZ || o.open[b] < kOpenBinMin) continue;
        G& g = gs[ng];
        U32 qh = 0, qt = 0;
        queue[qt++] = (U8)b;
        group[b] = (U8)ng;
        while (qh < qt)
        {
            const U32 k = queue[qh++];
            g.d += o.dir[k] * o.open[k];
            g.sr += o.open[k] * kOpenBinSr;
            for (U32 j = 0; j < o.nn[k]; ++j)
            {
                const U32 q = o.nb[k][j];
                if (group[q] != 0xFF || !o.seen[q] || o.dir[q].mV[VZ] < kOpenFloorZ) continue;
                if (o.open[q] >= kOpenBinMin)
                {
                    group[q] = (U8)ng;
                    queue[qt++] = (U8)q;
                }
                else if (o.wall[q] > 0.f)
                {
                    g.wd += o.wall[q];   // (a closed neighbour: the wall the opening sits in)
                    g.wn += 1.f;
                }
            }
        }
        ++ng;
    }
    // The largest kMaxOpenings.
    U32 order[kOpenBins];
    for (U32 i = 0; i < ng; ++i) order[i] = i;
    std::sort(order, order + ng, [&gs](U32 a, U32 b) { return gs[a].sr > gs[b].sr; });
    o.n = 0;
    for (U32 k = 0; k < ng && o.n < LLCleanAcoustics::kMaxOpenings; ++k)
    {
        const G& g = gs[order[k]];
        LLVector3 d = g.d;
        if (d.normalize() < 1e-3f) continue;
        LLCleanAcoustics::Opening& op = o.out[o.n++];
        op.dir = d;
        op.solid = g.sr;
        op.dist = llclamp(g.wn > 0.f ? g.wd / g.wn : o.room, 0.5f, range);
    }
}

// LITE MODE harvest (AudioAcoustics inactive): only the room rays, only while wanted.
void openingsLite(F64 now)
{
    OpenState& o = ost();
    if (now > o.wanted_until) return;
    static LLVKAcousticsResult s_res;   // (large: kept off the stack)
    if (LLVulkanContext::instance().latestAcoustics(s_res) && s_res.valid) openingsFromRoom(s_res, now);
}
} // namespace

// static
void LLCleanAcoustics::fillRequest(LLVKWorldFrame& wf)
{
    LLVKAcousticsRequest& r = wf.acoustics;
    r = LLVKAcousticsRequest{};
    State& s = st();
    const F64 now = LLFrameTimer::getElapsedSeconds();
    // #894 LITE MODE: no EFX room model running, but the weather wind audio wants the openings
    // around the listener: a room-only request (no sources, no path queries).
    const bool lite = !s.active && gAudiop && gAgent.getRegion() && now <= ost().wanted_until
                   && LLVulkanContext::instance().rayTracingEnabled();
    if (!lite && (!s.active || !gAudiop || gAudiop != s.engine || !gAgent.getRegion())) return;

    const LLVector3 ear = gAgent.getPosAgentFromGlobal(earGlobal());
    if (++sSerial == 0) sSerial = 1;
    r.valid = true;
    r.serial = sSerial;
    r.listener[0] = ear.mV[VX]; r.listener[1] = ear.mV[VY]; r.listener[2] = ear.mV[VZ];
    {
        static LLCachedControl<U32> s_interval(gSavedSettings, "AudioAcousticsRoomInterval", 3);
        static LLCachedControl<F32> s_maxdist(gSavedSettings, "AudioAcousticsRoomMaxDist", 30.f);
        static LLCachedControl<F32> s_foliage(gSavedSettings, "AudioAcousticsFoliageTestDist", 15.f);
        static LLCachedControl<bool> s_alpha(gSavedSettings, "AudioAcousticsAlphaBlocks", true);
        sAlphaBlocks = s_alpha;
        r.alpha_blocks = sAlphaBlocks;
        sRoomRange = llclamp((F32)s_maxdist, 5.f, 256.f);
        r.room_range = sRoomRange;
        r.room_interval = llclamp((U32)s_interval, 1u, 600u);
        r.foliage_test_dist = llclamp((F32)s_foliage, 0.f, 10000.f);
        static U32 sLogInterval = 0;
        static F32 sLogRange = -1.f, sLogFoliage = -1.f;
        static S32 sLogAlpha = -1;
        if (sLogInterval != r.room_interval || sLogRange != r.room_range || sLogFoliage != r.foliage_test_dist
            || sLogAlpha != (S32)r.alpha_blocks)
        {
            sLogInterval = r.room_interval; sLogRange = r.room_range; sLogFoliage = r.foliage_test_dist;
            sLogAlpha = (S32)r.alpha_blocks;
            LL_INFOS("Acoustics") << "acoustics GPU cost settings: room rays every " << sLogInterval
                                  << " frame(s), room ray max distance " << sLogRange
                                  << " m, foliage alpha-test distance " << sLogFoliage << " m, alpha blocks "
                                  << (r.alpha_blocks ? "on (visible alpha-blend = glass)" : "off (alpha-blend passes)")
                                  << LL_ENDL;
        }
    }
    // #930 LYING SNOW: only while the weather is on and snow lies (or falls); 0 = today's probe.
    {
        static LLCachedControl<F32> s_snow(gSavedSettings, "AudioAcousticsSnowAbsorb", 1.f);
        const F32 k = llclamp((F32)s_snow, 0.f, 1.f);
        const bool master = SLWeather::masterEnabled();
        const SLWeather::State& ws = SLWeather::instance().state();
        const bool lies = master && k > 0.f && (ws.snowCover > SLWeather::kEpsilon || ws.snowDepth > SLWeather::kEpsilon);
        r.snow_strength = lies ? k : 0.f;
        r.snow_depth = lies ? std::max(ws.snowDepth, 0.f) : 0.f;
        sSnowWet = lies ? llclamp(ws.wetness, 0.f, 1.f) : 0.f;
        sSnowFall = (master && k > 0.f) ? k * smooth01(0.5f, 1.f, ws.snow) : 0.f;
    }
    if (lite) return;   // (#894: the room rays only)
    // #683 early gain: which request's bounce sets went out last (the bounce pass runs on its own
    // frames): their sources are in flight until answered (or kEarlyHold).
    {
        const U32 disp = LLVulkanContext::instance().acousticsBounceDispatched();
        if (disp != 0 && disp != s.eg_last_disp)
        {
            s.eg_last_disp = disp;
            const Pending& pd = s.ring[disp % kRing];
            if (pd.serial == disp)
                for (U32 j = 0; j < pd.eg_n; ++j)
                {
                    auto it = s.src.find(pd.ids[pd.eg_k[j]]);
                    if (it != s.src.end()) it->second.eg_disp = now;
                }
        }
    }
    Pending& p = s.ring[r.serial % kRing];
    p.serial = r.serial;
    p.n = 0;
    p.eg_n = 0;
    p.listener = ear;
    for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS && p.n < kMaxSrc; ++i)
    {
        LLAudioChannel* ch = gAudiop->getChannelAt(i);
        LLAudioSource* src = ch ? ch->getSource() : nullptr;
        if (!LLAudioEngine::acousticsEligible(src)) continue;
        const LLVector3 pos = gAgent.getPosAgentFromGlobal(src->getPositionGlobal());
        const F32 d = dist_vec(pos, ear);
        if (d > kMaxSourceDist) continue;
        // The ray stops short of the emitter by about its own size: a sound plays from its
        // prim's centre, and the prim's own faces must not count as a wall.
        F32 margin = 0.5f;
        LLViewerObject* obj = gObjectList.findObject(src->getID());
        if (obj)
        {
            if (!obj->isAvatar()) margin = llclamp(0.5f * obj->getScale().length(), 0.3f, 4.f);
        }
        r.sources[p.n][0] = pos.mV[VX];
        r.sources[p.n][1] = pos.mV[VY];
        r.sources[p.n][2] = pos.mV[VZ];
        r.sources[p.n][3] = margin;
        p.ids[p.n] = src->getID();
        p.pos[p.n] = pos;
        SrcState& t = track(s, p.ids[p.n], now);
        t.seen = now;
        t.prio = src->getGain() / std::max(d, 1.f);
        t.pos = pos;
        t.margin = margin;
        // (log) what it is: an attachment (its item name when it is ours), an avatar, a prim, or
        // a triggered one-shot (no object: every trigger is a new source id); + its sound's id.
        if (s.early_on)
        {
            std::string kind = !obj ? "trigger" : obj->isAvatar() ? "avatar" : obj->isAttachment() ? "att" : "prim";
            if (obj && obj->isAttachment())
            {
                const std::string& item = obj->getAttachmentItemName();
                if (!item.empty()) kind += ":" + item.substr(0, 12);
            }
            if (LLAudioData* ad = src->getCurrentData()) kind += " " + ad->getID().asString().substr(0, 4);
            snprintf(t.label, sizeof(t.label), "%s", kind.c_str());
        }
        ++p.n;
    }
    // The channel-less sources (thunder, speakers): always room for them (static_assert above).
    for (const Extra& e : sExtra)
    {
        if (p.n >= kMaxSrc) break;
        if (e.key.isNull() || now >= e.until || dist_vec(e.pos, ear) > kMaxSourceDist) continue;
        r.sources[p.n][0] = e.pos.mV[VX];
        r.sources[p.n][1] = e.pos.mV[VY];
        r.sources[p.n][2] = e.pos.mV[VZ];
        r.sources[p.n][3] = 0.5f;
        p.ids[p.n] = e.key;
        p.pos[p.n] = e.pos;
        SrcState& t = track(s, e.key, now);
        t.seen = now;
        t.prio = 1.f / std::max(dist_vec(e.pos, ear), 1.f);
        t.pos = e.pos;
        t.margin = 0.5f;
        if (!t.label[0]) snprintf(t.label, sizeof(t.label), "speaker/thunder");
        ++p.n;
    }
    r.source_count = p.n;
    s.traced = p.n;

    // ---- SCHEDULING: #857 source-room probes, #858 path queries + listener shadow rays ----
    // Coverage first: every traced source is probed at least every kProbeLow, the most audible
    // every kProbeTop; the kTopOccluded most audible occluded sources get their cached paths
    // re-validated every frame and discovery every kDiscoverTop, the others every
    // kValidateLow / kDiscoverLow. Within a budget the most overdue (relative to its own
    // interval) goes first, so a shortfall stretches everyone evenly instead of starving some.
    static LLCachedControl<U32> s_rooms(gSavedSettings, "AudioAcousticsSourceRooms", 8);
    static LLCachedControl<U32> s_rays(gSavedSettings, "AudioAcousticsSourceRoomRays", 32);
    static LLCachedControl<U32> s_paths(gSavedSettings, "AudioAcousticsPathQueries", 64);
    static LLCachedControl<F32> s_diff(gSavedSettings, "AudioAcousticsDiffraction", 1.f);
    static LLCachedControl<U32> s_diff_src(gSavedSettings, "AudioAcousticsDiffractionSources", 4);
    U32 order[kMaxSrc];
    for (U32 k = 0; k < p.n; ++k) order[k] = k;
    std::sort(order, order + p.n, [&](U32 a, U32 b) { return s.src[p.ids[a]].prio > s.src[p.ids[b]].prio; });
    {
        U32 n_top = 0;
        for (U32 rank = 0; rank < p.n; ++rank)
        {
            SrcState& t = s.src[p.ids[order[rank]]];
            t.top = t.occ_valid && t.occ > 0.2f && n_top < kTopOccluded;
            if (t.top) ++n_top;
        }
    }
    struct Cand { U32 k; F32 score; };
    auto byScore = [](const Cand& a, const Cand& b) { return a.score > b.score; };
    auto overdue = [&](F64 last, F64 interval, F32 prio) -> F32
    {
        if (last <= 0.0) return 1e6f + prio;                 // never: first
        if (interval <= 0.0) return 1e5f + prio;             // every opportunity
        return (F32)((now - last) / interval);
    };
    const F32 range = sRoomRange;
    const LLVector3 L = ear;
    p.nq = 0;
    s.probe_n = s.q_probe = s.q_leg = s.q_guide = s.q_join = 0;
    auto addQ = [&](U8 type, U32 si) -> LLVKAcousticsQuery*
    {
        if (p.nq >= kMaxQueries) return nullptr;
        p.meta[p.nq] = QMeta{};
        p.meta[p.nq].type = type;
        p.meta[p.nq].src = (U16)si;
        r.queries[p.nq] = LLVKAcousticsQuery{};
        return &r.queries[p.nq++];
    };
    auto setRay = [](LLVKAcousticsQuery& q, const LLVector3& o, const LLVector3& d, F32 rng_m, F32 tmin, F32 miss,
                     const LLVector3& tgt, F32 tmargin)
    {
        q.a[0] = o.mV[VX]; q.a[1] = o.mV[VY]; q.a[2] = o.mV[VZ]; q.a[3] = rng_m;
        q.b[0] = tgt.mV[VX]; q.b[1] = tgt.mV[VY]; q.b[2] = tgt.mV[VZ]; q.b[3] = tmargin;
        q.d[0] = d.mV[VX]; q.d[1] = d.mV[VY]; q.d[2] = d.mV[VZ]; q.d[3] = tmin;
        q.e[0] = miss;
    };
    auto setSeg = [](LLVKAcousticsQuery& q, const LLVector3& a, const LLVector3& b, F32 bmargin)
    {
        q.a[0] = a.mV[VX]; q.a[1] = a.mV[VY]; q.a[2] = a.mV[VZ]; q.a[3] = -1.f;
        q.b[0] = b.mV[VX]; q.b[1] = b.mV[VY]; q.b[2] = b.mV[VZ]; q.b[3] = bmargin;
    };

    // (1) #857 source-room probes: kProbePer rays from the source over the sphere (Fibonacci,
    // randomly rotated), each end point tested against the listener.
    const bool rooms_on = (U32)s_rooms > 0;
    const U32 probe_budget = rooms_on ? std::min<U32>((U32)s_rays / kProbePer * kProbePer, kMaxQueries) : 0u;
    if (probe_budget > 0)
    {
        Cand c[kMaxSrc];
        U32 nc = 0;
        for (U32 rank = 0; rank < p.n; ++rank)
        {
            const SrcState& t = s.src[p.ids[order[rank]]];
            const F32 sc = overdue(t.probe_req, rank < (U32)s_rooms ? kProbeTop : kProbeLow, t.prio);
            if (sc >= 1.f) c[nc++] = Cand{order[rank], sc};
        }
        std::sort(c, c + nc, byScore);
        for (U32 ci = 0; ci < nc && p.nq + kProbePer <= probe_budget; ++ci)
        {
            const U32 k = c[ci].k;
            SrcState& t = s.src[p.ids[k]];
            const LLVector3& S = p.pos[k];
            const F32 D = dist_vec(S, L);
            const F32 oz = rngF(s), oa = rngF(s);
            for (U32 j = 0; j < kProbePer; ++j)
            {
                const F32 z = 1.f - 2.f * ((F32)j + oz) / (F32)kProbePer;
                const F32 ph = F_TWO_PI * (((F32)j * 0.61803398875f + oa) - std::floor((F32)j * 0.61803398875f + oa));
                const F32 rr = std::sqrt(std::max(0.f, 1.f - z * z));
                const LLVector3 dir(rr * std::cos(ph), rr * std::sin(ph), z);
                LLVKAcousticsQuery* q = addQ(Q_PROBE, k);
                setRay(*q, S, dir, range, llclamp(t.margin, 0.05f, 1.5f), std::min(range, std::max(D, 2.f)), L, 0.05f);
                p.meta[p.nq - 1].dir_z = z;
                p.meta[p.nq - 1].dir_x = dir.mV[VX];
                p.meta[p.nq - 1].dir_y = dir.mV[VY];
            }
            t.probe_req = now;
            ++s.probe_n;
            s.q_probe += kProbePer;
        }
    }

    // (2) #858 path queries: validation of the kept paths first (at most 3/4 of the budget),
    // then guided discovery and two-bounce joins for occluded sources.
    const bool diff_on = (F32)s_diff > 0.f;
    const U32 path_start = p.nq;
    const U32 path_limit = diff_on ? std::min<U32>(kMaxQueries, path_start + std::min<U32>(s_paths, kMaxQueries)) : path_start;
    if (path_limit > path_start)
    {
        const U32 val_limit = path_start + (path_limit - path_start) * 3 / 4;
        Cand c[kMaxSrc];
        U32 nc = 0;
        for (U32 rank = 0; rank < p.n; ++rank)
        {
            const SrcState& t = s.src[p.ids[order[rank]]];
            bool any = false;
            for (const Path& pa : t.paths) any |= pa.uid != 0;
            if (!any) continue;
            const F32 sc = overdue(t.val_req, t.top ? kValidateTop : kValidateLow, t.prio);
            if (sc >= 1.f) c[nc++] = Cand{order[rank], sc};
        }
        std::sort(c, c + nc, byScore);
        for (U32 ci = 0; ci < nc; ++ci)
        {
            const U32 k = c[ci].k;
            SrcState& t = s.src[p.ids[k]];
            const LLVector3& S = p.pos[k];
            bool all = true;
            // (Starting where the last cut-short pass stopped: no path starves.)
            for (U32 pi = 0; pi < kCachePaths; ++pi)
            {
                const Path& pa = t.paths[(t.val_rot + pi) % kCachePaths];
                if (!pa.uid) continue;
                const U32 legs = pa.bounces == 2 ? 3u : 2u;
                if (p.nq + legs > val_limit)
                {
                    t.val_rot = (U8)((t.val_rot + pi) % kCachePaths);
                    all = false;
                    break;
                }
                const LLVector3 pts[4] = {L, pa.p1, pa.bounces == 2 ? pa.p2 : S, S};
                for (U32 g = 0; g < legs; ++g)
                {
                    LLVKAcousticsQuery* q = addQ(Q_LEG, k);
                    setSeg(*q, pts[g], pts[g + 1], g + 1 == legs ? t.margin : 0.f);
                    p.meta[p.nq - 1].uid = pa.uid;
                    p.meta[p.nq - 1].legs = (U8)legs;
                }
                s.q_leg += legs;
            }
            if (!all) break;
            t.val_req = now;
        }

        // Discovery. The listener's live far points (openings), farthest first.
        U32 lf[kFarPoints];
        U32 nlf = 0;
        for (U32 i = 0; i < kFarPoints; ++i)
            if (farLive(s.lfar[i], now)) lf[nlf++] = i;
        std::sort(lf, lf + nlf, [&](U32 a, U32 b) { return s.lfar[a].d > s.lfar[b].d; });
        nc = 0;
        for (U32 rank = 0; rank < p.n; ++rank)
        {
            const SrcState& t = s.src[p.ids[order[rank]]];
            if (!(t.occ_valid && t.occ > 0.2f)) continue;
            const F32 sc = overdue(t.disc_req, t.top ? kDiscoverTop : kDiscoverLow, t.prio);
            if (sc >= 1.f) c[nc++] = Cand{order[rank], sc};
        }
        std::sort(c, c + nc, byScore);
        for (U32 ci = 0; ci < nc && p.nq + 2 * kGuided <= path_limit; ++ci)
        {
            const U32 k = c[ci].k;
            SrcState& t = s.src[p.ids[k]];
            const LLVector3& S = p.pos[k];
            const F32 D = dist_vec(S, L);
            if (D < 0.2f) continue;
            // Importance: the kept paths' points, the openings, and the straight line.
            LLVector3 axL[kCachePaths + kFarPoints + 1], axS[kCachePaths + kSrcFar + 1];
            F32 coneL[kCachePaths + kFarPoints + 1], coneS[kCachePaths + kSrcFar + 1];
            U32 nL = 0, nS = 0;
            for (const Path& pa : t.paths)
            {
                if (!pa.uid) continue;
                LLVector3 d = pa.p1 - L;
                if (d.normalize() > 1e-3f) { axL[nL] = d; coneL[nL++] = 0.15f; }
                LLVector3 e = (pa.bounces == 2 ? pa.p2 : pa.p1) - S;
                if (e.normalize() > 1e-3f) { axS[nS] = e; coneS[nS++] = 0.15f; }
            }
            for (U32 i = 0; i < nlf && i < 4; ++i)
            {
                LLVector3 d = s.lfar[lf[i]].p - L;
                if (d.normalize() > 1e-3f) { axL[nL] = d; coneL[nL++] = 0.12f; }
            }
            for (const FarPt& f : t.far_s)
            {
                if (!farLive(f, now)) continue;
                LLVector3 e = f.p - S;
                if (e.normalize() > 1e-3f) { axS[nS] = e; coneS[nS++] = 0.12f; }
            }
            const LLVector3 toS = (S - L) / D;
            for (U32 j = 0; j < kGuided; ++j)
            {
                // The last ray of each end always samples a wide cone round the straight line.
                const bool wide = j + 1 == kGuided || nL == 0;
                const U32 a = wide ? 0u : rngNext(s) % nL;
                const LLVector3 dl = wide ? coneDir(s, toS, 0.6f) : coneDir(s, axL[a], coneL[a]);
                LLVKAcousticsQuery* q = addQ(Q_GUIDE_L, k);
                setRay(*q, L, dl, range, 0.05f, std::min(range, D), S, t.margin);
                const bool wide_s = j + 1 == kGuided || nS == 0;
                const U32 b = wide_s ? 0u : rngNext(s) % nS;
                const LLVector3 ds = wide_s ? coneDir(s, -toS, 0.6f) : coneDir(s, axS[b], coneS[b]);
                q = addQ(Q_GUIDE_S, k);
                setRay(*q, S, ds, range, llclamp(t.margin, 0.05f, 1.5f), std::min(range, std::max(D, 2.f)), L, 0.05f);
            }
            s.q_guide += 2 * kGuided;
            // Two-bounce joins when no one-bounce path is known: listener far point (seen by L)
            // -> source far point (seen by S, not by L).
            if (livePaths(t, now, true) == 0)
            {
                U32 sf[kSrcFar];
                U32 nsf = 0;
                for (U32 i = 0; i < kSrcFar; ++i)
                    if (farLive(t.far_s[i], now)) sf[nsf++] = i;
                std::sort(sf, sf + nsf, [&](U32 a, U32 b) { return t.far_s[a].d > t.far_s[b].d; });
                U32 joins = 0;
                for (U32 i = 0; i < std::min<U32>(nlf, 2) && joins < kJoinsPerSource; ++i)
                {
                    for (U32 j = 0; j < std::min<U32>(nsf, 2) && joins < kJoinsPerSource; ++j)
                    {
                        if (p.nq >= path_limit) break;
                        LLVKAcousticsQuery* q = addQ(Q_JOIN, k);
                        setSeg(*q, s.lfar[lf[i]].p, t.far_s[sf[j]].p, 0.f);
                        p.meta[p.nq - 1].aux = t.far_s[sf[j]].p;
                        ++joins;
                    }
                }
                s.q_join += joins;
            }
            t.disc_req = now;
        }
    }
    r.query_count = p.nq;
    s.q_sum += p.nq;
    ++s.q_frames;

    // (3) #858 listener shadow rays (room frames): the most overdue occluded sources, the top
    // ones every kDiscoverTop, the rest every kDiscoverLow.
    s.diff_n = 0;
    const U32 dmax = diff_on ? std::min<U32>(s_diff_src, kMaxDiff) : 0u;
    if (dmax > 0)
    {
        Cand c[kMaxSrc];
        U32 nc = 0;
        for (U32 rank = 0; rank < p.n; ++rank)
        {
            const SrcState& t = s.src[p.ids[order[rank]]];
            if (!(t.occ_valid && t.occ > 0.2f)) continue;
            c[nc++] = Cand{order[rank], overdue(t.diff_at, t.top ? kDiscoverTop : kDiscoverLow, t.prio)};
        }
        std::sort(c, c + nc, byScore);
        for (U32 k = 0; k < std::min(nc, dmax); ++k) r.diff_src[k] = c[k].k;
        s.diff_n = std::min(nc, dmax);
    }

    // (4) #683 phase 1 BOUNCE PASS: the listener's set, the material table, and the sources in
    // another room most overdue for their own (see "MEASURED REVERB").
    static LLCachedControl<bool> s_bounce(gSavedSettings, "AudioAcousticsBounce", false);
    static LLCachedControl<U32> s_brays(gSavedSettings, "AudioAcousticsBounceRays", 512);
    static LLCachedControl<U32> s_bmax(gSavedSettings, "AudioAcousticsBounces", 6);
    static LLCachedControl<U32> s_bsrc(gSavedSettings, "AudioAcousticsBounceSources", 2);
    static LLCachedControl<U32> s_bsrc_rays(gSavedSettings, "AudioAcousticsBounceSourceRays", 128);
    static LLCachedControl<F32> s_bias_req(gSavedSettings, "AudioAcousticsAbsorptionBias", 0.f);
    // #683 early gain (see "EARLY GAIN"): source sets toward the listener; they need the bounce
    // dispatch (and its material table) even with the listener's own set off.
    static LLCachedControl<bool> s_eg(gSavedSettings, "AudioAcousticsEarlyGain", false);
    static LLCachedControl<U32> s_eg_src(gSavedSettings, "AudioAcousticsEarlyGainSources", 8);
    static LLCachedControl<U32> s_eg_rays(gSavedSettings, "AudioAcousticsEarlyGainRays", 128);
    static LLCachedControl<U32> s_eg_bounces(gSavedSettings, "AudioAcousticsEarlyGainBounces", 3);
    s.bounce_on = s_bounce;
    s.early_on = s_eg && (U32)s_eg_src > 0;
    s.bounce_src_n = 0;
    s.early_src_n = 0;
    if (s.bounce_on || s.early_on)
    {
        r.bounce = true;
        r.bounce_listener = s.bounce_on;
        r.bounce_rays = llclamp((U32)s_brays, 64u, LLVKAcousticsRequest::kMaxBounceListenerRays);
        r.bounce_max = llclamp((U32)s_bmax, 1u, LLVKAcousticsRequest::kMaxBounces);
        r.bounce_range = llclamp(2.f * sRoomRange, 30.f, 256.f);
        r.receiver_radius = kRecvR;
        r.speed = kSpeed;
        for (U32 b = 0; b < 3; ++b) r.air[b] = airPerM(b);
        // Classes: the surface + the room's clutter (soft furnishings no tag tells us about; as
        // the room model, gated by enclosure), the owner's bias, + kBounceUnmodelled.
        const F32 bmul = std::exp2(1.5f * llclamp((F32)s_bias_req, -1.f, 1.f));
        const F32 clut = s.room_valid ? s.L.clutter * enclosedWeight(s.L.enclosure) : 0.f;
        auto absorb = [&](F32 a, F32 clutter_k) { return llclamp((a + clutter_k * clut) * bmul + kBounceUnmodelled, 0.f, 0.95f); };
        for (U32 c = 0; c < BC_COUNT; ++c)
        {
            const BClass& bc = kBClass[c];
            r.bounce_class[c][0] = absorb(bc.low, 0.5f * kClutterMid);
            r.bounce_class[c][1] = absorb(bc.mid, kClutterMid);
            r.bounce_class[c][2] = absorb(bc.hf, kClutterHf);
            r.bounce_class[c][3] = bc.scatter;
        }
        // #930: the snow classes at the current wetness (the probe blends a snowy hit toward them).
        for (U32 c = BC_SNOW; c <= BC_SNOW_PACKED; ++c)
        {
            const SnowAbs sa = snowAbs(c == BC_SNOW_PACKED ? 1.f : 0.f);
            r.bounce_class[c][0] = absorb(sa.low, 0.5f * kClutterMid);
            r.bounce_class[c][1] = absorb(sa.mid, kClutterMid);
            r.bounce_class[c][2] = absorb(sa.hf, kClutterHf);
            r.bounce_class[c][3] = sa.scatter;
        }
        r.hard_abs = absorb(kAbsHard, kClutterMid);
        r.polished_scatter = kPolishedScatter;
        // The material table: the live cache entries (the newest when there are too many).
        {
            U32 idx[kMatSlots];
            U32 n = 0;
            for (U32 i = 0; i < kMatSlots; ++i)
                if (sMat[i].inst < 0xFFFFFFFEu && now - sMat[i].at <= kMatAge) idx[n++] = i;
            if (n > LLVKAcousticsRequest::kBounceMatMax)
            {
                std::nth_element(idx, idx + LLVKAcousticsRequest::kBounceMatMax, idx + n,
                                 [](U32 a, U32 b) { return sMat[a].at > sMat[b].at; });
                n = LLVKAcousticsRequest::kBounceMatMax;
            }
            for (U32 i = 0; i < n; ++i)
            {
                r.bounce_mat_inst[i] = sMat[idx[i]].inst;
                r.bounce_mat_class[i] = sMat[idx[i]].cls;
            }
            r.bounce_mat_count = n;
        }
        // Sources in ANOTHER room (#857 coupling), among the AudioAcousticsSourceRooms most audible,
        // with a fresh room probe: the K whose bounce answer is oldest (relative to kBounceSrcEvery).
        const U32 kmax = (s.bounce_on && rooms_on)
                             ? std::min<U32>(std::min<U32>((U32)s_bsrc, (U32)s_rooms), LLVKAcousticsRequest::kMaxBounceSets - 1)
                             : 0u;
        if (kmax > 0)
        {
            Cand c[kMaxSrc];
            U32 nc = 0;
            for (U32 rank = 0; rank < p.n && rank < (U32)s_rooms; ++rank)
            {
                const SrcState& t = s.src[p.ids[order[rank]]];
                if (!t.room_valid || now - t.probe_at > kProbeStaleLo) continue;
                if (smooth01(kCoupleLo, kCoupleHi, t.couple) >= 0.5f) continue;   // (same room: the listener's set covers it)
                const F32 sc = overdue(t.bounce_at, kBounceSrcEvery, t.prio);
                if (sc >= 1.f) c[nc++] = Cand{order[rank], sc};
            }
            std::sort(c, c + nc, byScore);
            for (U32 k = 0; k < std::min(nc, kmax); ++k)
            {
                LLVKAcousticsRequest::BounceSource& b = r.bounce_src[k];
                const SrcState& t = s.src[p.ids[c[k].k]];
                b.src = c[k].k;
                b.pos[0] = p.pos[c[k].k].mV[VX];
                b.pos[1] = p.pos[c[k].k].mV[VY];
                b.pos[2] = p.pos[c[k].k].mV[VZ];
                b.margin = t.margin;
                b.rays = llclamp((U32)s_bsrc_rays, 64u, LLVKAcousticsRequest::kMaxBounceSourceRays);
            }
            r.bounce_src_count = std::min(nc, kmax);
            s.bounce_src_n = r.bounce_src_count;
        }
        // #683 EARLY GAIN: the free source slots go to the AudioAcousticsEarlyGainSources most
        // audible sources, most overdue (since the last answer or dispatch, relative to
        // kEarlyEvery) first; one still in flight is skipped.
        const U32 free_slots = LLVKAcousticsRequest::kMaxBounceSets - 1 - r.bounce_src_count;
        if (s.early_on && free_slots > 0)
        {
            const F32 max_d = 0.9f * r.bounce_range;
            Cand c[kMaxSrc];
            U32 nc = 0;
            for (U32 rank = 0; rank < p.n && rank < (U32)s_eg_src; ++rank)
            {
                const U32 k = order[rank];
                const SrcState& t = s.src[p.ids[k]];
                const F32 D = dist_vec(p.pos[k], L);
                if (D < kEarlyMinDist || D > max_d) continue;
                if (t.eg_disp > t.eg_acc_at && now - t.eg_disp < kEarlyHold) continue;   // (in flight)
                // A sound without a confident measurement of its own yet (just started: a
                // one-shot, a new clip) takes every bounce frame until it has one, ahead of the
                // round robin (never measured: first of all).
                const F64 every = t.eg_den < kEarlyRaysHi ? 0.0 : kEarlyEvery;
                const F32 sc = overdue(std::max(t.eg_acc_at, t.eg_disp), every, t.prio);
                if (sc >= 1.f) c[nc++] = Cand{k, sc};
            }
            std::sort(c, c + nc, byScore);
            const U32 ne = std::min(nc, free_slots);
            const U32 rays = llclamp((U32)s_eg_rays, 64u, LLVKAcousticsRequest::kMaxBounceSourceRays);
            const U32 nb = llclamp((U32)s_eg_bounces, 1u, LLVKAcousticsRequest::kMaxBounces);
            for (U32 j = 0; j < ne; ++j)
            {
                const U32 k = c[j].k;
                LLVKAcousticsRequest::BounceSource& b = r.bounce_src[r.bounce_src_count + j];
                b = LLVKAcousticsRequest::BounceSource{};
                b.src = k;
                b.pos[0] = p.pos[k].mV[VX];
                b.pos[1] = p.pos[k].mV[VY];
                b.pos[2] = p.pos[k].mV[VZ];
                b.margin = s.src[p.ids[k]].margin;
                s.src[p.ids[k]].eg_start = llclamp(b.margin, 0.05f, 1.5f);   // (log: the emitter start radius, as the VK side clamps it)
                b.rays = rays;
                b.to_listener = true;
                b.recv_r = earlyRecvRadius(dist_vec(p.pos[k], L));
                b.bounces = nb;
                p.eg_k[p.eg_n++] = k;
            }
            r.bounce_src_count += ne;
            s.early_src_n = ne;
        }
    }
}

// static
bool LLCleanAcoustics::listenerRoom(F32& enclosure, F32& roof)
{
    const State& s = st();
    if (!s.active || !s.room_valid) return false;
    enclosure = s.L.enclosure;
    roof = s.L.roof;
    return true;
}

// static
bool LLCleanAcoustics::ownsUnderwater()
{
    const State& s = st();
    return s.active && s.uw_on;
}

// static
void LLCleanAcoustics::wantOpenings()
{
    ost().wanted_until = LLFrameTimer::getElapsedSeconds() + kOpenWantHold;
}

// static
void LLCleanAcoustics::cancelOpenings()
{
    OpenState& o = ost();
    o.wanted_until = 0.0;
    o.have = false;
    o.n = 0;
}

// static
bool LLCleanAcoustics::listenerOpenings(Opening out[kMaxOpenings], U32& n, F32& escape)
{
    const OpenState& o = ost();
    n = 0;
    if (!o.have || LLFrameTimer::getElapsedSeconds() - o.last_at > kOpenStale) return false;
    n = o.n;
    for (U32 i = 0; i < n; ++i) out[i] = o.out[i];
    escape = o.escape;
    return true;
}

// static
bool LLCleanAcoustics::listenerRoofMaterial(F32 shares[ROOF_MAT_COUNT])
{
    const State& s = st();
    if (!s.active || !s.room_valid) return false;
    for (U32 m = 0; m < ROOF_MAT_COUNT; ++m) shares[m] = s.roof_mat[m];
    return true;
}

// static
void LLCleanAcoustics::traceExtraSource(const LLUUID& key, const LLVector3& pos_agent)
{
    if (key.isNull()) return;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    Extra* slot = nullptr;
    for (Extra& e : sExtra)
        if (e.key == key) { slot = &e; break; }
    if (!slot)
    {
        // A free (expired) entry, else the one that expires first.
        for (Extra& e : sExtra)
            if (!slot || e.until < slot->until) slot = &e;
        if (slot->until > now && slot->key.notNull())
            LL_DEBUGS("Acoustics") << "extra source table full: " << slot->key << " replaced" << LL_ENDL;
    }
    slot->key = key;
    slot->pos = pos_agent;
    slot->until = now + kExtraHold;
}

// static
bool LLCleanAcoustics::occlusionOf(const LLUUID& key, F32& occ)
{
    const State& s = st();
    if (!s.active) return false;
    auto it = s.src.find(key);
    if (it == s.src.end() || !it->second.occ_valid) return false;
    occ = it->second.occ;
    return true;
}

// static
bool LLCleanAcoustics::sourceAcoustics(const LLUUID& key, SourceParams& out)
{
    const State& s = st();
    if (!s.active) return false;
    auto it = s.src.find(key);
    if (it == s.src.end() || !it->second.occ_valid) return false;
    const SrcState& t = it->second;
    out.direct_gain = t.out_gain;
    out.direct_gain_hf = t.out_hf;
    out.send_gain = t.out_send;
    out.send_gain_hf = t.out_send_hf;
    out.zone = t.zone;
    out.zone_gain = t.zone_send * t.zone_fade;
    out.zone_gain_hf = t.out_zone_hf;
    out.pan_weight = t.pan_valid ? t.pan_w : 0.f;
    out.pan_dir = t.pan_dir;
    out.bass_db = s.bass_avail ? t.bass_db : 0.f;
    out.direct_boost = t.out_boost;
    return true;
}

void LLCleanAcoustics::update()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "AudioAcoustics", false);
    static LLCachedControl<F32> s_level(gSavedSettings, "AudioAcousticsReverbLevel", 0.6f);
    static LLCachedControl<F32> s_strength(gSavedSettings, "AudioAcousticsOcclusion", 0.8f);
    static LLCachedControl<F32> s_bias(gSavedSettings, "AudioAcousticsAbsorptionBias", 0.f);
    static LLCachedControl<F32> s_send_occ(gSavedSettings, "AudioAcousticsSendOcclusion", 1.f);
    static LLCachedControl<U32> s_rooms(gSavedSettings, "AudioAcousticsSourceRooms", 8);
    static LLCachedControl<U32> s_zones(gSavedSettings, "AudioAcousticsZones", 2);
    static LLCachedControl<F32> s_diff(gSavedSettings, "AudioAcousticsDiffraction", 1.f);
    static LLCachedControl<F32> s_pan(gSavedSettings, "AudioAcousticsDiffractionPan", 0.6f);
    static LLCachedControl<F32> s_eg_window(gSavedSettings, "AudioAcousticsEarlyGainWindowMs", 50.f);
    static LLCachedControl<F32> s_eg_max(gSavedSettings, "AudioAcousticsEarlyGainMaxDb", 9.f);
    static LLCachedControl<F32> s_eg_share(gSavedSettings, "AudioAcousticsEarlyGainReverbShare", 0.9f);
    State& s = st();
    if (gAudiop != s.engine)
    {
        // Engine created / destroyed / restarted: every EFX object belonged to the old one.
        s = State{};
        s.engine = gAudiop;
        egCacheClear();
    }
    if (!gAudiop) return;
    if (!LLAudioEngine::sAcousticsKeepDry)
    {
        // HUD attachments play "at" the avatar but belong to the screen: keep them dry.
        LLAudioEngine::sAcousticsKeepDry = [](const LLAudioSource* src)
        {
            const LLViewerObject* obj = gObjectList.findObject(src->getID());
            return obj && obj->isHUDAttachment();
        };
    }

    const bool rt = LLVulkanContext::instance().rayTracingEnabled();
    const bool want = s_on && rt;
    if (!s_on)
    {
        s.failed = false;      // a later toggle on tries again
        s.warned_rt = false;
    }
    else if (!rt && !s.warned_rt)
    {
        s.warned_rt = true;
        LL_WARNS("Acoustics") << "AudioAcoustics is on but ray tracing is not (RenderVKRayTracing, a GPU with "
                                 "VK_KHR_ray_query): acoustics stays inert" << LL_ENDL;
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (want && !s.active && !s.failed)
    {
        s.active = gAudiop->setAcousticsEnabled(true);
        if (!s.active)
        {
            s.failed = true;
            LL_WARNS("Acoustics") << "AudioAcoustics: the audio engine has no usable EFX; acoustics stays inert" << LL_ENDL;
        }
        else
        {
            s.room_valid = false;
            s.src.clear();
            s.bass_wanted = s.bass_avail = false;   // (#897: nor a bass slot)
            s.LBnd = Boundary{};
            s.med_valid = false;                  // (#953: the ear's medium is taken as it is)
            s.zones_avail = s.zones_wanted = 0;   // (a fresh EFX path has no zone slots)
            for (Zone& z : s.zones) z = Zone{};
            s.last_answer = now;   // (the stale timer starts now)
            s.last_room_answer = 0.0;
            s.last_update = 0.0;
            s.next_log = now + kLogEvery;
            s.answers = s.frames_old_sum = 0;
            LL_INFOS("Acoustics") << "AudioAcoustics on: room reverb + occlusion from GPU rays (reverb level "
                                  << (F32)s_level << ", occlusion " << (F32)s_strength << "; source rooms "
                                  << (U32)s_rooms << ", zones " << (U32)s_zones << ", diffraction " << (F32)s_diff
                                  << ", pan " << (F32)s_pan << ")" << LL_ENDL;
        }
    }
    else if (!want && s.active)
    {
        gAudiop->setAcousticsEnabled(false);   // filters + sends detached, slots deleted
        s.active = false;
        s.src.clear();
        s.room_valid = false;
        s.bass_wanted = s.bass_avail = false;
        s.zones_avail = s.zones_wanted = 0;
        for (Zone& z : s.zones) z = Zone{};
        resetChannels();
        LL_INFOS("Acoustics") << "AudioAcoustics off: plain audio path" << LL_ENDL;
    }
    if (!s.active)
    {
        openingsLite(now);   // #894: the weather's room-only requests, when it asked for them
        return;
    }
    if (!gAudiop->acousticsActive())
    {
        s.active = false;   // (the engine dropped it underneath: try again only after a toggle)
        s.failed = true;
        return;
    }

    const F32 dt = s.last_update > 0.0 ? llclamp((F32)(now - s.last_update), 0.f, 0.5f) : 0.f;
    s.last_update = now;
    s.level = s_level;
    s.bias = s_bias;
    const bool rooms_on = (U32)s_rooms > 0;

    // ---- #857 zone slots: only while source rooms run (0 = no slot exists) ----
    {
        const U32 wanted = rooms_on ? std::min<U32>(s_zones, kMaxZones) : 0u;
        if (wanted != s.zones_wanted)
        {
            s.zones_wanted = wanted;
            s.zones_avail = gAudiop->setAcousticsZoneCount(wanted);
            for (U32 z = s.zones_avail; z < kMaxZones; ++z) s.zones[z] = Zone{};
            if (wanted > 0 && s.zones_avail < wanted)
                LL_INFOS("Acoustics") << "reverb zones: " << s.zones_avail << " of " << wanted
                                      << " available (the device needs 2 auxiliary sends per source)" << LL_ENDL;
        }
    }

    // ---- #897 the boundary-bass slot: while AudioAcousticsBoundaryBass > 0 ----
    static LLCachedControl<F32> s_bass(gSavedSettings, "AudioAcousticsBoundaryBass", 1.f);
    const F32 bass_strength = llclamp((F32)s_bass, 0.f, 1.f);
    if ((bass_strength > 0.f) != s.bass_wanted)
    {
        s.bass_wanted = bass_strength > 0.f;
        s.bass_avail = gAudiop->setAcousticsBass(s.bass_wanted);
    }

    // ---- the newest GPU answer ----
    LLVKAcousticsResult res;
    if (LLVulkanContext::instance().latestAcoustics(res) && res.valid && (S32)(res.serial - s.last_serial) > 0)
    {
        const F32 adt = llclamp((F32)(now - s.last_answer), 0.f, 0.5f);
        s.last_serial = res.serial;
        s.last_answer = now;
        ++s.answers;
        s.frames_old_sum += res.frames_old;
        occFromAnswer(s, res, adt, now);
        if (res.room_serial != 0 && res.room_serial != s.last_room_serial)
        {
            // Room rays arrive at the lower rate: smooth by the real seconds since the last ones.
            const F32 rdt = s.last_room_answer > 0.0 ? llclamp((F32)(now - s.last_room_answer), 0.f, 2.f) : 0.f;
            s.last_room_serial = res.room_serial;
            s.last_room_answer = now;
            roomFromAnswer(s, res, rdt);
            farFromRoom(s, res, now);
            openingsFromRoom(res, now);   // #894 (the weather wind audio's openings)
            diffFromAnswer(s, res, now);
        }
        queriesFromAnswer(s, res, now);
    }
    // ---- #683 phase 1: the newest bounce answer (copied only when new) ----
    const F32 eg_window = 0.001f * llclamp((F32)s_eg_window, 0.f, 200.f);
    if (s.bounce_on || s.early_on)
    {
        static LLVKAcousticsBounce s_bres;   // (large: kept off the stack)
        if (LLVulkanContext::instance().latestAcousticsBounce(s_bres, s.last_bounce_serial) && s_bres.valid)
        {
            s.last_bounce_serial = s_bres.serial;
            ++s.win_bounce;
            s.bounce_wg = s_bres.workgroups;
            s.bounce_frames_old = s_bres.frames_old;
            s.bounce_max = s_bres.bounces;
            const Pending& p = s.ring[s_bres.serial % kRing];
            for (U32 k = 0; k < s_bres.set_count; ++k)
            {
                const LLVKAcousticsBounceSet& b = s_bres.set[k];
                if (b.src == LLVKAcousticsRequest::kNone)
                {
                    accumulate(s.LB, b, s_bres.bounces, now);
                    analyseBounce(s.LB, s.LM, now);
                    s.win_bounce_rays += (F32)b.rays;
                    continue;
                }
                if (p.serial != s_bres.serial || b.src >= p.n) continue;
                const LLUUID& id = p.ids[b.src];
                auto it = s.src.find(id);
                if (it == s.src.end()) continue;
                if (b.to_listener)
                {
                    // #683 early gain: the reflected energy reaching the listener from this source.
                    accumulateEarly(it->second, b, eg_window, now);
                    ++s.win_early_sets;
                    continue;
                }
                // Its accumulator (the least recently used one gives way).
                State::SrcBounce* slot = nullptr;
                for (State::SrcBounce& e : s.sb)
                    if (e.id == id) { slot = &e; break; }
                if (!slot)
                {
                    for (State::SrcBounce& e : s.sb)
                        if (!slot || e.used < slot->used) slot = &e;
                    slot->id = id;
                    slot->acc = BounceAcc{};
                }
                slot->used = now;
                accumulate(slot->acc, b, s_bres.bounces, now);
                analyseBounce(slot->acc, it->second.meas, now);
                it->second.bounce_at = now;
                ++s.win_bounce_src;
            }
        }
    }
    // No answers (RT warming up / TLAS not built / probe off): drift toward dry + clear
    // rather than holding a stale room forever.
    if (now - s.last_answer > kStaleAfter)
    {
        const F32 a = follow(kStaleTau, dt);
        s.L.enclosure = lerpf(s.L.enclosure, 0.f, a);
        s.L.roof = lerpf(s.L.roof, 0.f, a);
        for (auto& kv : s.src) kv.second.occ = lerpf(kv.second.occ, 0.f, a);
    }
    // Forget sources not traced for 5 s (bounded: kMaxTracked, and only recent voices).
    for (auto it = s.src.begin(); it != s.src.end();)
    {
        if (now - it->second.seen > 5.0 && now - s.last_answer < kStaleAfter) it = s.src.erase(it);
        else ++it;
    }

    // ---- per source: direct filter, room sends, zone want, pan (also the #892 query) ----
    // #683 early gain: how much of a same-room source's early field the shared reverb plays
    // already (the listener's room-ness, as reverbFromRoom, x the reverb level), and the cap.
    const F32 eg_share = llclamp((F32)s_eg_share, 0.f, 1.f);
    const F32 eg_room = s.room_valid ? smooth01(0.25f, 0.85f, 0.7f * s.L.enclosure + 0.3f * s.L.roof)
                                           * llclamp(s.level / 0.3f, 0.f, 1.f)
                                     : 0.f;
    const F32 eg_cap = std::min(std::pow(10.f, llclamp((F32)s_eg_max, 0.f, 12.f) / 20.f), LLAudioEngine::kAcousticsMaxBoost);
    const F32 strength = llclamp((F32)s_strength, 0.f, 1.f);
    const F32 send_w = llclamp((F32)s_send_occ, 0.f, 1.f);
    const F32 diff = llclamp((F32)s_diff, 0.f, 1.f);
    const F32 pan = llclamp((F32)s_pan, 0.f, 1.f);
    const LLVector3 ear_agent = gAgent.getPosAgentFromGlobal(earGlobal());
    // #953 the water (see "WATER"): the ear's medium (boundary loss) and the underwater profile.
    static LLCachedControl<F32> s_wb(gSavedSettings, "AudioAcousticsWaterBoundaryLoss", 30.f);
    static LLCachedControl<F32> s_wb_hf(gSavedSettings, "AudioAcousticsWaterBoundaryLossHF", 10.f);
    static LLCachedControl<bool> s_uw(gSavedSettings, "AudioAcousticsUnderwater", true);
    const F32 wb_db = llclamp((F32)s_wb, 0.f, 60.f);
    const F32 wb_hf_db = llclamp((F32)s_wb_hf, 0.f, 40.f);
    s.uw_on = s_uw;
    {
        const F32 lw = inWater(ear_agent);
        if (!s.med_valid)
        {
            s.lmed = lw;
            s.uw = s.uw_on ? lw : 0.f;
            s.med_valid = true;
        }
        else
        {
            glideTo(s.lmed, lw, follow(kMediumTau, dt));
            glideTo(s.uw, s.uw_on ? lw : 0.f, follow(kUwTau, dt));
        }
    }
    const F32 uw_hf = lerpf(1.f, kUwHearHf, s.uw);   // underwater hearing (1 in air)
    {
        // (#683 early gain: the spatial cache is in agent space -- a new region invalidates it.)
        static U64 s_eg_region = 0;
        const U64 rh = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
        if (rh != s_eg_region) { egCacheClear(); s_eg_region = rh; }
    }
    for (auto& kv : s.src)
    {
        SrcState& t = kv.second;
        if (!t.occ_valid) continue;
        const F32 o = t.occ;
        // #858: the kept, validated paths take back their share of the muffling.
        evaluatePaths(s, t, ear_agent, now, dt, diff);
        const F32 og = o * (1.f - t.q);
        const F32 oh = o * (1.f - t.q_hf);
        // Conservative: never below -14 dB overall; the highs go first.
        t.out_gain = llclamp(1.f - strength * 0.55f * og, 0.2f, 1.f);
        t.out_hf = llclamp(1.f - strength * 0.9f * oh, 0.05f, 1.f);
        // Listener-room send. Without a source room: the old rule (the send filtered like
        // the direct path once clearly occluded; 0 = blocked sounds ring the room).
        const F32 sw = send_w * llclamp(o * 2.f, 0.f, 1.f);
        F32 sg = lerpf(1.f, t.out_gain, sw), shf = lerpf(1.f, t.out_hf, sw);
        t.zone_want = 0.f;
        t.same = 1.f - llclamp(o * 2.f, 0.f, 1.f);
        // #857 with a fresh source room: the share of its sound that lands in the listener's
        // space (coupling) excites the listener's room in full, the rest only by the path that
        // reaches the listener (walls, or a doorway the #858 paths found: out_gain).
        // A pillar in the same room no longer darkens the room send; a sound outdoors seen
        // through the door rings the room only by what the door lets in.
        // (Ramped in over the first ~0.75 s of probes: no step in the send when a blocked
        // sound enters the probed set, and a few probes average before coupling counts.)
        const F32 rw = (rooms_on && t.room_valid)
                           ? (1.f - smooth01((F32)kProbeStaleLo, (F32)kProbeStaleHi, (F32)(now - t.probe_at)))
                                 * smooth01(0.f, 0.75f, (F32)(now - t.first_probe_at))
                           : 0.f;
        if (rw > 0.f)
        {
            const F32 same = smooth01(kCoupleLo, kCoupleHi, t.couple);
            const F32 eg = same + (1.f - same) * t.out_gain;
            const F32 eh = same + (1.f - same) * t.out_hf;
            sg = lerpf(sg, lerpf(1.f, eg, send_w), rw);
            shf = lerpf(shf, lerpf(1.f, eh, send_w), rw);
            t.same = lerpf(t.same, same, rw);
            // Its own room's reverb, heard through whatever path reaches the listener.
            t.zone_want = rw * (1.f - same) * t.wet * t.out_gain;
        }
        t.out_send = sg;
        t.out_send_hf = shf;
        t.out_zone_hf = t.out_hf;
        // #683 EARLY GAIN (see "EARLY GAIN"): the reflected energy reaching the listener from this
        // source, the part the shared reverb does not already play, added to the direct level
        // against the free-field direct energy. Multipliers on today's gain / HF, exactly 1 with
        // nothing measured (or the setting off).
        {
            F32 want_m = 1.f, want_hf = 1.f;
            t.eg_share = 0.f;
            // The spatial cache: a sound with nothing (fresh) of its own takes the newest
            // measurement made between (about) the same emitter and listener positions. A NEW
            // sound seeded this way starts at its level (no attack ramp: a one-shot may be over
            // before the ramp is).
            if (s.early_on && (t.eg_den <= 0.f || now - t.eg_acc_at > kEarlyStaleLo))
            {
                if (const EgCacheEntry* e = egCacheFind(t.pos, ear_agent, now, false))
                {
                    if (e->at > t.eg_acc_at && e->den > 0.f)
                    {
                        for (U32 b = 0; b < 3; ++b) t.eg_num[b] = e->num[b];
                        t.eg_den = e->den;
                        t.eg_acc_at = now;   // (it fades from now unless its own answers follow)
                        t.eg_cached = true;
                        if (now - t.first_seen < 0.5 && t.eg_mult == 1.f) t.eg_jump = true;
                    }
                }
            }
            const F32 fresh = (s.early_on && t.eg_den > 0.f)
                                  ? (1.f - smooth01((F32)kEarlyStaleLo, (F32)kEarlyStaleHi, (F32)(now - t.eg_acc_at)))
                                        * smooth01(kEarlyRaysLo, kEarlyRaysHi, t.eg_den)
                                  : 0.f;
            if (fresh > 0.f)
            {
                for (U32 b = 0; b < 3; ++b) t.eg_ratio[b] = std::max(t.eg_num[b] / t.eg_den, 0.f);
                const F32 w = fresh * (1.f - eg_share * eg_room * t.same);
                const F32 rm = w * t.eg_ratio[1], rh = w * t.eg_ratio[2];
                // Today's direct (with diffraction) and the same without the #858 paths.
                const F32 g = t.out_gain, g0 = llclamp(1.f - strength * 0.55f * o, 0.2f, 1.f);
                const F32 a = g * t.out_hf, a0 = g0 * llclamp(1.f - strength * 0.9f * o, 0.05f, 1.f);
                const F32 xm = std::max(rm - (g * g - g0 * g0), 0.f);   // (max(diffraction, early))
                const F32 xh = std::max(rh - (a * a - a0 * a0), 0.f);
                if (xm > 0.f || xh > 0.f)
                {
                    const F32 em = 1.f + xm / (g * g);    // E / g^2
                    const F32 eh = 1.f + xh / (a * a);    // E_hf / a^2
                    want_m = std::min(std::sqrt(em), eg_cap);
                    want_hf = std::min(std::sqrt(eh / em), 1.f / std::max(t.out_hf, 0.05f));
                    t.eg_share = xm / (g * g + xm);
                }
            }
            // Smoothed in dB (up kEarlyAttack, down kEarlyRelease); back to exactly 1 once there.
            auto glide = [&](F32& v, F32 target)
            {
                const F32 lv = std::log(std::max(v, 1e-3f)), lt = std::log(std::max(target, 1e-3f));
                const F32 nv = std::exp(lerpf(lv, lt, follow(lt > lv ? kEarlyAttack : kEarlyRelease, dt)));
                v = (target == 1.f && std::fabs(nv - 1.f) < 1e-3f) ? 1.f : nv;
            };
            if (t.eg_jump)
            {
                t.eg_mult = want_m;
                t.eg_hf = want_hf;
                t.eg_jump = false;
            }
            glide(t.eg_mult, want_m);
            glide(t.eg_hf, want_hf);
            const F32 level = t.out_gain * t.eg_mult;
            t.out_boost = std::max(level, 1.f);
            t.out_gain = level / t.out_boost;
            t.out_hf = llclamp(t.out_hf * t.eg_hf, 0.05f, 1.f);
        }
        // #953 the AIR / WATER BOUNDARY (see "WATER"): after everything else, on every path the
        // source has to the listener (direct, listener-room send, zone send); + the underwater
        // hearing on the highs. Exactly today's on the same side in air.
        {
            const F32 sw = inWater(t.pos);
            if (!t.med_valid)
            {
                t.med = sw;   // (a source starts at its value: no fade-in from full level)
                t.med_valid = true;
            }
            else
                glideTo(t.med, sw, follow(kMediumTau, dt));
            F32 bg = 1.f, bh = 1.f;
            boundaryLoss(t.med, s.lmed, wb_db, wb_hf_db, bg, bh, &t.med_x);
            // (The hearing part on the direct path only: the reverb slots' GAINHF carries it for
            // the sends -- underwaterReverb --, so it is not taken twice.)
            t.out_gain *= bg;
            t.out_hf = llclamp(t.out_hf * bh * uw_hf, 0.f, 1.f);
            t.out_send *= bg;
            t.out_send_hf = llclamp(t.out_send_hf * bh, 0.f, 1.f);
            t.zone_want *= bg;
            t.out_zone_hf = llclamp(t.out_zone_hf * bh, 0.f, 1.f);
        }
        // #858 pan: an occluded source heard by an indirect path seems to come from it -- only
        // when the paths agree on a direction (one doorway / corner), not when they scatter
        // over the room (a pillar between you in the same room: keep the true direction).
        F32 wp = 0.f;
        if (pan > 0.f && diff > 0.f && t.ind_dir_valid && t.n_valid > 0)
        {
            wp = pan * o * smooth01(0.05f, 0.35f, t.q) * smooth01(0.3f, 0.7f, t.conc);
            t.pan_dir = t.pan_valid ? nlerp(t.pan_dir, t.ind_dir, follow(kPanTau, dt)) : t.ind_dir;
            t.pan_valid = true;
        }
        t.pan_w = lerpf(t.pan_w, wp, follow(kPanTau, dt));
        // #897 boundary bass: the listener's surroundings + the source's (fresh probe only).
        F32 bass = 0.f;
        if (s.bass_avail)
        {
            bass = boundaryDb(s.LBnd);
            if (t.bnd.valid && now - t.probe_at <= kProbeStaleHi) bass += boundaryDb(t.bnd);
            bass = std::min(bass, LLAudioEngine::kAcousticsBassMaxDb) * bass_strength;
        }
        t.bass_db = lerpf(t.bass_db, bass, follow(kBassTau, dt));
    }
    updateZones(s, dt, now);

    // ---- apply to the channels ----
    U32 occluded = 0, eligible = 0, sends_filtered = 0, panned = 0, bass_on = 0;
    const LLVector3d ear = earGlobal();
    for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
    {
        LLAudioChannel* ch = gAudiop->getChannelAt(i);
        if (!ch) continue;
        LLAudioSource* src = ch->getSource();
        const auto it = LLAudioEngine::acousticsEligible(src) ? s.src.find(src->getID()) : s.src.end();
        if (!LLAudioEngine::acousticsEligible(src) || it == s.src.end() || !it->second.occ_valid)
        {
            // #953: an in-world voice not answered yet still takes the boundary loss and the
            // underwater hearing (no sound across the surface starting at full level).
            F32 bg = 1.f, bh = 1.f, hear = 1.f;
            if (LLAudioEngine::acousticsEligible(src))
            {
                boundaryLoss(inWater(gAgent.getPosAgentFromGlobal(src->getPositionGlobal())), s.lmed, wb_db, wb_hf_db, bg, bh);
                hear = uw_hf;
            }
            ch->setAcousticsDirect(bg, bh * hear);
            ch->setAcousticsSend(bg, bh);
            ch->setAcousticsZone(LLUUID::null, -1, 0.f, 1.f);
            ch->setAcousticsPosition(LLUUID::null, false, LLVector3d());
            ch->setAcousticsBass(LLUUID::null, 0.f);
            ch->setAcousticsBoost(LLUUID::null, 1.f);
            if (LLAudioEngine::acousticsEligible(src)) ++eligible;
            continue;
        }
        ++eligible;
        const SrcState& t = it->second;
        if (t.occ > 0.5f) ++occluded;
        ch->setAcousticsDirect(t.out_gain, t.out_hf);
        ch->setAcousticsSend(t.out_send, t.out_send_hf);
        if (t.out_send < 0.99f || t.out_send_hf < 0.99f) ++sends_filtered;
        const S32 zone = (t.zone >= 0 && (U32)t.zone < s.zones_avail) ? t.zone : -1;
        ch->setAcousticsZone(src->getID(), zone, t.zone_send * t.zone_fade, t.out_zone_hf);
        ch->setAcousticsBass(src->getID(), s.bass_avail ? t.bass_db : 0.f);   // #897
        ch->setAcousticsBoost(src->getID(), t.out_boost);                     // #683 early gain
        if (s.bass_avail && t.bass_db > 0.5f) ++bass_on;
        // Virtual position: the true distance, the direction blended toward the opening.
        bool pos_on = false;
        LLVector3d vpos;
        if (t.pan_valid && t.pan_w > 0.01f)
        {
            const LLVector3d rel = src->getPositionGlobal() - ear;
            const F64 D = rel.length();
            if (D > 0.1)
            {
                const LLVector3 tdir((F32)(rel.mdV[VX] / D), (F32)(rel.mdV[VY] / D), (F32)(rel.mdV[VZ] / D));
                const LLVector3 a = nlerp(tdir, t.pan_dir, t.pan_w);
                vpos = ear + LLVector3d(a) * D;
                pos_on = true;
                ++panned;
            }
        }
        ch->setAcousticsPosition(src->getID(), pos_on, vpos);
    }

    // ---- the reverbs (20 Hz; the engine reloads an effect only on a real change) ----
    if (now - s.reverb_push >= 0.05)
    {
        s.reverb_push = now;
        const F32 lb = measAge(s.LM, now);   // #683 phase 1
        s.reverb = reverbFromRoom(s.L, s_level, s_bias, &s.derived, &s.LM, lb);
        // Early / late pans from the measured arrival directions, in the listener's frame (EAX:
        // x right, y up, z forward), by the decay confidence; the late pan only when the tail
        // really comes from one side.
        const F32 bd = s.derived.blend_decay;
        if (bd > 0.f)
        {
            LLVector3 at = gAudiop->getListenerAt(), up = gAudiop->getListenerUp();
            LLVector3 right = at % up;
            if (at.normVec() > 0.f && right.normVec() > 0.f)
            {
                up = right % at;
                auto toEax = [&](const LLVector3& d, F32 mag, F32 out[3])
                {
                    out[0] = (d * right) * mag;
                    out[1] = (d * up) * mag;
                    out[2] = (d * at) * mag;
                };
                toEax(s.LM.early_dir, bd * s.LM.early_conc, s.reverb.reflections_pan);
                const F32 lc = smooth01(0.1f, 0.4f, s.LM.late_conc) * s.LM.late_conc;
                toEax(s.LM.late_dir, bd * lc, s.reverb.late_pan);
            }
        }
        // #953: the ear in the water hears its (water-filled) room at the speed of sound in water.
        if (s.uw > 0.f) s.reverb = underwaterReverb(s.reverb, s.uw, s.uw);
        gAudiop->setAcousticsReverb(s.reverb);
        for (U32 z = 0; z < s.zones_avail; ++z)
        {
            Zone& zn = s.zones[z];
            // The members' sends carry their own wet level (their room's), so the slot is at 1.
            zn.rv = reverbFromRoom(zn.room, 1.f, s_bias, nullptr, &zn.meas, measAge(zn.meas, now));
            if (s.uw > 0.f) zn.rv = underwaterReverb(zn.rv, 0.f, s.uw);   // (#953: the hearing part only)
            zn.rv.wet = 1.f;
            gAudiop->setAcousticsZoneReverb(z, zn.rv);
        }
    }

    // ---- log (~10 s) ----
    if (now >= s.next_log)
    {
        s.next_log = now + kLogEvery;
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        if (s.answers == 0)
        {
            os << "no GPU answers in the last " << (S32)kLogEvery << " s (TLAS not built / RT warming up?)";
        }
        else
        {
            const HitCounts& hc = s.hc;
            const F32 nh = (F32)std::max<U32>(hc.hits, 1u);
            const F32 nr = (F32)std::max<U32>(hc.rays, 1u);
            os << "enclosure " << s.L.enclosure << " roof " << s.L.roof << " escape " << s.L.escape << " ("
               << std::setprecision(0) << (100.f * hc.escapes / nr) << "% of rays) room " << std::setprecision(1)
               << s.L.room_size << " m (hits " << s.hit_size << " m) | absorption " << std::setprecision(2) << s.derived.eff_mid
               << " (HF " << s.derived.eff_hf << "; surfaces " << s.L.surf_mid << ", clutter " << s.L.clutter << " x"
               << enclosedWeight(s.L.enclosure) << ", bias " << (F32)s_bias << "; hits " << std::setprecision(0)
               << (100.f * hc.hard / nh) << "% hard " << (100.f * hc.soft / nh) << "% soft "
               << (100.f * hc.foliage / nh) << "% foliage " << (100.f * hc.glass / nh) << "% glass "
               << (100.f * hc.terrain / nh) << "% terrain " << (100.f * hc.water / nh) << "% water "
               << (100.f * hc.snow / nh) << "% snow)"
               << std::setprecision(2) << ", open-area total " << s.derived.tot_mid << " (HF " << s.derived.tot_hf << ")"
               << " -> decay " << std::setprecision(2) << s.reverb.decay_time << " s, HF ratio "
               << s.reverb.decay_hf_ratio << ", wet " << s.reverb.wet << ", early " << s.reverb.reflections_gain
               << " | voices " << eligible << " (" << s.traced << " traced), " << occluded << " occluded (" << s.occ_glass << " through glass only), " << sends_filtered << " sends filtered";
            // #930 lying snow: how much of the room lies under snow and what it did to each output.
            {
                static LLCachedControl<F32> s_snow_log(gSavedSettings, "AudioAcousticsSnowAbsorb", 1.f);
                F32 src_wet = 0.f, src_n = 0.f;
                for (const auto& kv : s.src)
                    if (kv.second.room_valid && now - kv.second.probe_at <= kProbeStaleHi) { src_wet += kv.second.wet; src_n += 1.f; }
                os << " | snow: absorb x" << std::setprecision(2) << (F32)s_snow_log << ", " << std::setprecision(0)
                   << (100.f * hc.snow / nh) << "% of room hits (share-weighted), wetness " << std::setprecision(2) << sSnowWet
                   << ", heavy snowfall " << sSnowFall << "; room wet: model " << s.derived.stat_wet << " (room-constant x"
                   << s.derived.stat_room_k << ")";
                if (s.derived.blend > 0.f)
                    os << ", measured " << s.derived.meas_wet << " (containment " << s.derived.meas_cont << ", room-constant x"
                       << s.derived.meas_room_k << " = open " << s.derived.meas_open << " + surfaces " << s.derived.meas_a_area
                       << " (bounce energy-weighted " << s.derived.meas_a_energy << "), blend " << s.derived.blend << ")";
                os << " -> " << s.reverb.wet << ", decay " << s.reverb.decay_time << " s (model " << s.derived.stat_decay
                   << "), early refl " << s.reverb.reflections_gain << "; source rooms mean wet "
                   << (src_n > 0.f ? src_wet / src_n : 0.f) << " (" << (S32)src_n << ")";
            }
            // #953 the water: the ear's medium, the profile, the sources across the surface.
            {
                U32 across = 0, in_water = 0;
                for (const auto& kv : s.src)
                {
                    if (!kv.second.occ_valid || !kv.second.med_valid) continue;
                    if (kv.second.med_x > 0.5f) ++across;
                    if (kv.second.med > 0.5f) ++in_water;
                }
                os << " | water: ear in water " << std::setprecision(2) << s.lmed << ", underwater profile " << s.uw
                   << (s.uw_on ? "" : " (off)") << ", " << in_water << " sources in water, " << across
                   << " across the surface (boundary -" << std::setprecision(0) << wb_db << " dB, HF -"
                   << (wb_db + wb_hf_db) << " dB)" << std::setprecision(2);
            }
            // #857 source rooms + zones.
            U32 with_room = 0, other = 0;
            F32 couple_sum = 0.f;
            for (const auto& kv : s.src)
            {
                const SrcState& t = kv.second;
                if (!t.occ_valid || !t.room_valid || now - t.probe_at > kProbeStaleHi) continue;
                ++with_room;
                couple_sum += t.couple;
                if (t.same < 0.5f) ++other;
            }
            os << " | source rooms: " << with_room << " fresh (" << s.probe_n << " probed/frame, " << s.win_probes
               << " probes, " << std::setprecision(0)
               << (s.win_probe_rays ? 100.f * s.win_probe_vis / (F32)s.win_probe_rays : 0.f) << "% end points heard"
               << std::setprecision(2) << "), mean coupling " << (with_room ? couple_sum / with_room : 0.f) << ", "
               << other << " in another room; zones " << s.zones_avail << "/" << s.zones_wanted;
            for (U32 z = 0; z < s.zones_avail; ++z)
            {
                const Zone& zn = s.zones[z];
                F32 send_max = 0.f;
                for (const auto& kv : s.src)
                    if (kv.second.zone == (S32)z) send_max = std::max(send_max, kv.second.zone_send * kv.second.zone_fade);
                os << " [z" << z << (zn.active ? "" : " idle") << ": " << zn.members << " src, send max " << send_max
                   << ", room " << std::setprecision(1) << zn.room.room_size << " m encl " << std::setprecision(2)
                   << zn.room.enclosure << " esc " << zn.room.escape << ", decay " << zn.rv.decay_time << " s]";
            }
            // #858 diffraction: the path caches, their validation, and the schedule's coverage.
            const F32 secs = s.win_start > 0.0 ? std::max((F32)(now - s.win_start), 1.f) : (F32)kLogEvery;
            U32 helped = 0, occ_n = 0, cached = 0, cached_2b = 0, with_paths = 0;
            F32 rho_sum = 0.f, q_max = 0.f, pan_max = 0.f;
            F32 page_max = 0.f, page_sum = 0.f, dage_max = 0.f, dage_sum = 0.f;
            U32 page_n = 0;
            for (const auto& kv : s.src)
            {
                const SrcState& t = kv.second;
                if (!t.occ_valid) continue;
                U32 live = 0;
                for (const Path& pa : t.paths)
                {
                    if (!pa.uid || now - pa.last_ok > kPathStale) continue;
                    ++live;
                    if (pa.bounces == 2) ++cached_2b;
                }
                cached += live;
                if (live) ++with_paths;
                // Selection age: since the last probe answer (every traced source) ...
                const F32 pa = t.probe_at > 0.0 ? (F32)(now - t.probe_at) : 99.f;
                page_max = std::max(page_max, pa);
                page_sum += pa;
                ++page_n;
                if (t.occ < 0.5f) continue;
                // ... and, occluded, since the last path evidence (validation or discovery).
                ++occ_n;
                const F64 last = std::max(std::max(t.val_at, t.disc_at), t.diff_at);
                const F32 da = last > 0.0 ? (F32)(now - last) : 99.f;
                dage_max = std::max(dage_max, da);
                dage_sum += da;
                q_max = std::max(q_max, t.q);
                pan_max = std::max(pan_max, t.pan_w);
                if (t.q > 0.05f)
                {
                    ++helped;
                    rho_sum += std::max(t.rho, 1.f);
                }
            }
            os << " | diffraction: " << helped << " of " << occ_n << " occluded with a path (mean detour x"
               << (helped ? rho_sum / helped : 0.f) << ", strength max " << q_max << "), pan max " << pan_max << " ("
               << panned << " panned); cached paths " << cached << " alive (" << cached_2b << " two-bounce) on "
               << with_paths << " sources, per s: " << std::setprecision(1) << s.win_valid_ok / secs << " re-validated, "
               << s.win_valid_lost / secs << " lost, " << s.win_expired / secs << " expired, " << s.win_found / secs
               << " found (" << s.win_found_2b / secs << " two-bounce); listener far points "
               << std::count_if(std::begin(s.lfar), std::end(s.lfar), [&](const FarPt& f) { return farLive(f, now); })
               << " live; listener shadow rays " << s.win_nee_found << "/"
               << s.win_nee << " with a path; queries/frame " << (s.q_frames ? (F32)s.q_sum / (F32)s.q_frames : 0.f)
               << " (now " << s.q_probe << " probe, " << s.q_leg << " validate, " << s.q_guide << " guided, " << s.q_join
               << " join; " << s.diff_n << " shadow-ray sources) | selection age probe max " << std::setprecision(2)
               << page_max << " s mean " << (page_n ? page_sum / page_n : 0.f) << " s, paths (occluded) max "
               << dage_max << " s mean " << (occ_n ? dage_sum / occ_n : 0.f) << " s";
            // The most audible occluded sources (the test case's line).
            {
                const SrcState* top[3] = {nullptr, nullptr, nullptr};
                const LLUUID* top_id[3] = {nullptr, nullptr, nullptr};
                for (const auto& kv : s.src)
                {
                    const SrcState& t = kv.second;
                    if (!t.occ_valid || t.occ < 0.5f) continue;
                    for (U32 k = 0; k < 3; ++k)
                    {
                        if (!top[k] || t.prio > top[k]->prio)
                        {
                            for (U32 m = 2; m > k; --m) { top[m] = top[m - 1]; top_id[m] = top_id[m - 1]; }
                            top[k] = &t;
                            top_id[k] = &kv.first;
                            break;
                        }
                    }
                }
                for (U32 k = 0; k < 3 && top[k]; ++k)
                {
                    const SrcState& t = *top[k];
                    os << " [" << top_id[k]->asString().substr(0, 8) << ": " << std::setprecision(1)
                       << dist_vec(t.pos, ear_agent) << " m, occ " << std::setprecision(2) << t.occ << ", paths "
                       << t.n_valid << ", strength " << t.q << ", detour x" << t.rho << ", coupling " << t.couple
                       << ", pan " << t.pan_w << ", zone " << t.zone << "]";
                }
            }
            // #897 boundary bass.
            {
                F32 lb_n = 0.f;
                const F32 lb_db = boundaryDb(s.LBnd, &lb_n);
                F32 src_max = 0.f;
                for (const auto& kv : s.src) src_max = std::max(src_max, kv.second.bass_db);
                os << " | boundary bass: " << (s.bass_avail ? "on" : (s.bass_wanted ? "unavailable" : "off"))
                   << std::setprecision(2) << " x" << bass_strength << ", listener " << lb_n << " boundaries +"
                   << std::setprecision(1) << lb_db << " dB (nearest";
                const char* axn[6] = {"-x", "+x", "-y", "+y", "-z", "+z"};
                for (U32 a = 0; a < 6; ++a) os << " " << axn[a] << " " << std::setprecision(2) << s.LBnd.d[a];
                os << " m), " << bass_on << " voices boosted, max +" << std::setprecision(1) << src_max << " dB";
            }
            // #683 early gain: sets this window / now, and the most boosted sources.
            if (s.early_on)
            {
                U32 boosted = 0, measured = 0;
                const SrcState* top[3] = {nullptr, nullptr, nullptr};
                const LLUUID* top_id[3] = {nullptr, nullptr, nullptr};
                for (const auto& kv : s.src)
                {
                    const SrcState& t = kv.second;
                    if (t.eg_mult > 1.06f) ++boosted;   // (> +0.5 dB)
                    if (t.eg_rays_win > 0.f) ++measured;
                    if (!(t.eg_den > 0.f)) continue;
                    for (U32 k = 0; k < 3; ++k)
                    {
                        if (!top[k] || t.eg_mult > top[k]->eg_mult)
                        {
                            for (U32 m = 2; m > k; --m) { top[m] = top[m - 1]; top_id[m] = top_id[m - 1]; }
                            top[k] = &t;
                            top_id[k] = &kv.first;
                            break;
                        }
                    }
                }
                os << " | early gain: " << s.win_early_sets << " sets on " << measured << " sources (" << s.early_src_n
                   << " in the newest request), " << boosted
                   << " sources over +0.5 dB, cap +" << std::setprecision(1) << 20.f * std::log10(eg_cap)
                   << " dB, reverb share " << std::setprecision(2) << eg_share << " x room " << eg_room;
                U32 cache_live = 0;
                for (const EgCacheEntry& e : sEgCache) cache_live += (e.at > 0.0 && now - e.at <= kEgCacheAge) ? 1u : 0u;
                os << ", spatial cache " << cache_live << "/" << kEgCacheSlots;
                for (U32 k = 0; k < 3 && top[k]; ++k)
                {
                    const SrcState& t = *top[k];
                    // (early share = the early part of the resulting level, xm / (g^2 + xm) -- NOT the
                    // reverb share; "kept" is the part of the measured energy the reverb does not already play:
                    // 1 - AudioAcousticsEarlyGainReverbShare x room x same.)
                    const F32 rw = std::max(t.eg_rays_win, 1.f);
                    os << " [" << top_id[k]->asString().substr(0, 8) << " " << t.label << ": " << std::setprecision(1)
                       << dist_vec(t.pos, ear_agent) << " m, " << (t.eg_mult >= 1.f ? "+" : "")
                       << 20.f * std::log10(std::max(t.eg_mult, 1e-3f)) << " dB, early share " << std::setprecision(2)
                       << t.eg_share << " (kept x" << (1.f - eg_share * eg_room * t.same) << "), early/direct low/mid/high "
                       << t.eg_ratio[0] << "/" << t.eg_ratio[1] << "/" << t.eg_ratio[2] << ", HF x" << t.eg_hf << ", same "
                       << t.same << ", boost x" << t.out_boost << ", start " << t.eg_start << " m, rr " << t.eg_rr
                       << " m, window " << std::setprecision(0) << 1000.f * t.eg_window << " ms, per ray (all times): hits "
                       << std::setprecision(2) << t.eg_hits_win / rw << " diffuse " << t.eg_nee_win / rw << " specular "
                       << std::setprecision(3) << t.eg_spec_win / rw << (t.eg_cached ? ", seeded from cache" : "") << ", "
                       << std::setprecision(0) << t.eg_rays_win / secs << " rays/s]";
                }
            }
            // #683 phase 1 bounce pass.
            if (s.bounce_on)
            {
                const Measured& M = s.LM;
                static LLCachedControl<bool> s_blog(gSavedSettings, "AudioAcousticsBounceLog", false);
                os << " | bounce: " << s.win_bounce << " answers (" << std::setprecision(0)
                   << (s.win_bounce ? s.win_bounce_rays / s.win_bounce : 0.f) << " listener rays x " << s.bounce_max
                   << " bounces, " << s.bounce_wg << " workgroups, " << s.bounce_frames_old << " frames old; "
                   << s.win_bounce_src << " source sets, " << s.bounce_src_n << " now)";
                if (M.valid)
                {
                    const char* band[3] = {"low", "mid", "high"};
                    os << std::setprecision(2) << ", conf " << M.conf << " (decay " << M.conf_decay << "), blend "
                       << s.derived.blend << " (decay " << s.derived.blend_decay << "); T60";
                    for (U32 b = 0; b < 3; ++b)
                    {
                        os << " " << band[b] << " " << M.t60[b] << " s (rate " << M.t_rate[b];
                        if (M.t_fit[b] > 0.f)
                            os << ", fit " << M.t_fit[b] << " over " << std::setprecision(0) << M.fit_span[b]
                               << " dB r2 " << std::setprecision(3) << M.fit_r2[b] << std::setprecision(2) << " w "
                               << M.fit_w[b];
                        else
                            os << ", no fit";
                        os << ", data covers " << std::setprecision(0) << M.covered[b] << " dB)" << std::setprecision(2);
                    }
                    os << "; statistical decay " << s.derived.stat_decay << " s wet " << s.derived.stat_wet
                       << "; containment " << M.containment << ", mfp " << std::setprecision(1) << M.mfp << " m, hits/ray "
                       << M.hits_per_ray << ", escaped " << std::setprecision(0) << (100.f * M.esc_frac) << "% capped "
                       << (100.f * M.cap_frac) << "%; early " << std::setprecision(1) << (1000.f * M.refl_delay)
                       << " ms gain " << std::setprecision(2) << M.refl_gain << " (E early/late " << M.e_early << "/"
                       << M.e_late << ") from (" << M.early_dir.mV[VX] << ", " << M.early_dir.mV[VY] << ", "
                       << M.early_dir.mV[VZ] << ") x" << M.early_conc << ", late +" << std::setprecision(1)
                       << (1000.f * M.late_delay) << " ms x" << std::setprecision(2) << M.late_conc << ", GAINHF " << M.gain_hf
                       << " GAINLF " << M.gain_lf << " -> EAX decay " << s.reverb.decay_time << " s HF "
                       << s.reverb.decay_hf_ratio << " LF " << s.reverb.decay_lf_ratio;
                    if (s_blog)
                    {
                        os << " [diffuse rain " << std::setprecision(1) << M.nee_per_ray << "/ray, specular "
                           << std::setprecision(3) << M.spec_per_ray << "/ray, rays in window " << std::setprecision(0)
                           << M.rays << "; pans refl (" << std::setprecision(2) << s.reverb.reflections_pan[0] << ", "
                           << s.reverb.reflections_pan[1] << ", " << s.reverb.reflections_pan[2] << ") late ("
                           << s.reverb.late_pan[0] << ", " << s.reverb.late_pan[1] << ", " << s.reverb.late_pan[2] << ")]";
                        for (const auto& kv : s.src)
                        {
                            const Measured& Q = kv.second.meas;
                            if (!Q.valid || now - Q.at > kMeasStaleHi) continue;
                            os << " [src " << kv.first.asString().substr(0, 8) << ": T60 " << Q.t60[0] << "/" << Q.t60[1]
                               << "/" << Q.t60[2] << " s, containment " << Q.containment << ", conf " << Q.conf_decay
                               << ", zone " << kv.second.zone << "]";
                        }
                    }
                }
                else
                    os << ", no measurement yet";
            }
            os << " | records " << s.src.size() << "/" << kMaxTracked << " (peak " << s.src_peak << ")"
               << " | " << s.answers << " answers, avg " << std::setprecision(1)
               << ((F32)s.frames_old_sum / (F32)s.answers) << " frames old";
        }
        LL_INFOS("Acoustics") << os.str() << LL_ENDL;
        s.answers = s.frames_old_sum = 0;
        s.hc = HitCounts{};
        s.win_probes = s.win_probe_vis = s.win_probe_rays = s.win_nee = s.win_nee_found = 0;
        s.win_valid_ok = s.win_valid_lost = s.win_expired = s.win_found = s.win_found_2b = 0;
        s.q_sum = 0;
        s.q_frames = 0;
        s.win_bounce = s.win_bounce_src = 0;
        s.win_bounce_rays = 0.f;
        s.win_early_sets = 0;
        for (auto& kv : s.src)
        {
            SrcState& t = kv.second;
            t.eg_rays_win = t.eg_hits_win = t.eg_nee_win = t.eg_spec_win = 0.f;
            t.eg_cached = false;
        }
        s.win_start = now;
    }
}
