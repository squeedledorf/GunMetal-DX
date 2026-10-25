/**
 * @file llcleanacoustics.h
 * @brief Acoustics prototype (#683 phase 0, AudioAcoustics): room reverb + occlusion
 *        muffling for in-world sounds, driven by GPU rays against the scene TLAS.
 *
 * Listener-centred and deliberately simple (the owner listens before anything grows):
 *
 *  - fillRequest (per frame, with the world frame): the listener (the OpenAL ear,
 *    MediaSoundsEarLocation: the camera by default) and every in-world voice on an OpenAL
 *    channel, agent space, into LLVKWorldFrame::acoustics. The renderer traces 64 rays over
 *    the sphere + one listener -> source ray per voice (vkgfx/llcleanaudiortvk.h) and
 *    answers ~3 frames later; no ray tracing = no answers = the feature stays dry.
 *  - update (per frame, audio idle): turns the engine's EFX path on / off with the setting
 *    (off = no EFX object exists: the plain audio path), harvests the newest answer and
 *    smooths it over seconds into
 *      * ENCLOSURE (hit fraction of the non-floor rays, foliage counting 0.25 of a wall,
 *        with a roof term) and ROOM SIZE (mean wall / ceiling distance, only as far as
 *        enclosed: open ground near scattered buildings is no big room) -> one EAX reverb:
 *        outdoors dry, a small room short, a hall long -- parameters interpolated, never
 *        preset jumps;
 *      * ABSORPTION over the WHOLE sphere: an escaping ray (sky / nothing in range) is open
 *        area, absorption 1; per hit, the prim's physical material (stone / metal / glass
 *        hard, flesh / rubber soft, wood -- the default -- a weak prior leaning hard), pulled
 *        harder by the render material's hint (metal, polished, env reflection); alpha-mask
 *        FOLIAGE (alpha-tested) is soft and scattering; visible alpha-blend (windows, glass
 *        walls / roofs; AudioAcousticsAlphaBlocks) is a hard glass wall; plus, when enclosed, the room's
 *        GEOMETRIC CLUTTER (rays landing in front of their neighbours' plane: furniture,
 *        logs, shelves) -> Eyring-style decay (open spaces short by themselves), HF ratio,
 *        early reflections, warmth and tail level (AudioAcousticsAbsorptionBias tilts it);
 *      * per source, the blocked fraction of its (jittered) ray -> a low-pass on its
 *        direct path (AudioAcousticsOcclusion); a ray crossing only a visible glass pane /
 *        hard alpha-mask screen counts as partly blocked (kOccGlass).
 *    Every ~10 s a log line (enclosure, escape fraction, room size, absorption + clutter,
 *    foliage share, decay, HF ratio, wet, sources occluded; source rooms / zones and
 *    diffraction below) so what is heard can be matched to what was measured.
 *
 *  - #857 SOURCE ROOMS (AudioAcousticsSourceRooms most audible sources, AudioAcousticsSourceRoomRays
 *    rays per frame, 16 per source in turn): each probed source gets its own room estimate
 *    (the listener's model, smoothed ~1 s per source) and a COUPLING: the share of its probe
 *    rays' end points the listener can see (a same-room source ~0.7-1, the next room through
 *    a door a few, sealed 0). Coupling sets a same-room weight: the listener-room send is the
 *    coupled share plus wall transmission for the rest (a pillar in your room no longer darkens
 *    the room send; a sound outside the door rings your room only by what the door lets in);
 *    the rest sends (second EFX send) to a REVERB ZONE configured from rooms like its own
 *    (AudioAcousticsZones extra slots, clustered with hysteresis; a retarget fades the send
 *    out and back in), at its own room's wet level times the path's transmission. The
 *    listener's room stays the main reverb. Without a fresh probe: the listener-only rule.
 *  - #858 DIFFRACTION (AudioAcousticsDiffraction, AudioAcousticsDiffractionSources,
 *    AudioAcousticsPathQueries): paths listener -> P1 (-> P2) -> source found by any ray (the
 *    listener's room hits tested against occluded sources, the source probes' end points,
 *    guided rays importance-sampled toward openings and earlier paths, two-bounce joins of a
 *    listener far point with a source far point) are KEPT per source and re-validated leg by
 *    leg with segment queries (every 50 ms for the 6 most audible occluded sources, 0.25 s for
 *    the rest): a doorway once found stays found, a door closing drops it in ~0.1-0.2 s.
 *    Every traced source is probed / searched on a deadline (0.2 / 0.5 s), most audible first.
 *    A path's strength falls with its detour (and bounces); it takes back that share of the muffling (HF less so for a
 *    sharp turn), and (AudioAcousticsDiffractionPan) pans the sound toward the opening when
 *    the paths agree on a direction -- a virtual position at the true distance, so the level
 *    does not jump. Everything is smoothed (attack / release, direction, send fades).
 *  - #683 PHASE 1 MEASURED REVERB (AudioAcousticsBounce, default off): multi-bounce energy rays
 *    on the GPU against our own TLAS (listener-centric, + source-centric for a few sources in
 *    another room) give a 3-band echogram and energy balance; per band the decay (the measured
 *    loss rate, pulled toward a Schroeder T20 / T30 fit where the data covers the decay), the
 *    containment (wet), early reflections (delay, level, direction) and the late onset / direction
 *    drive the same EAX reverb, blended over the statistical model by confidence. See
 *    "MEASURED REVERB" in llcleanacoustics.cpp.
 *  - #683 EARLY GAIN (AudioAcousticsEarlyGain, default off): source -> LISTENER bounce sets for the
 *    AudioAcousticsEarlyGainSources most audible sources (round robin) measure the reflected
 *    energy arriving within AudioAcousticsEarlyGainWindowMs of the direct sound; the part the
 *    shared reverb does not already play raises that source's direct level (against its
 *    free-field direct energy, so an occluded source cannot explode; capped at
 *    AudioAcousticsEarlyGainMaxDb; HF through the direct filter; exactly today's level when
 *    nothing reflects). New sounds are measured first, and a spatial cache (emitter + listener
 *    position) gives a one-shot / the next clip its level at once. See "EARLY GAIN" in
 *    llcleanacoustics.cpp.
 *  - #953 WATER: the room / probe rays reflect off the water surface (water absorption class,
 *    as the bounce pass); a source and the listener on opposite sides of the surface (region
 *    water height) lose AudioAcousticsWaterBoundaryLoss dB (+ ...LossHF at HF) on every path
 *    through the existing filters; an ear in the water (AudioAcousticsUnderwater) crossfades to
 *    an underwater profile (duller hearing, the room's reverb at the speed of sound in water) and
 *    owns the underwater distance rolloff (ownsUnderwater). See "WATER" in llcleanacoustics.cpp.
 *  - Records per source (occlusion + room + paths + zone) live while traced, 5 s grace, at
 *    most 64 (least recently traced evicted); live / cap / peak in the log.
 *  - #894 OPENINGS (listenerOpenings, for the weather wind audio): the room rays' escaped / far
 *    directions on 64 fixed bins, smoothed, grouped into up to 4 openings (direction, solid
 *    angle, distance). With AudioAcoustics off but ray tracing on, wantOpenings() makes
 *    fillRequest send room-only requests while the weather wants them (no EFX, no sources).
 *
 * UI / preview sounds, media, voice and the parcel stream's listener-relative stereo bed are
 * never touched (dry). The parcel stream played through in-world SPEAKERS (#892,
 * slparcelspeakeraudio.h) is in-world sound: each audible speaker (up to 32) is traced with
 * traceExtraSource under its own key and its engine source gets sourceAcoustics() -- occlusion
 * low-pass, room send, reverb zone, diffraction pan -- like a channel voice. The ray budget
 * (LLVKAcousticsRequest::kMaxSources = 64) holds all 30 channels plus 34 such sources, so the
 * speakers never take a channel voice's ray.
 */
#ifndef LL_LLCLEANACOUSTICS_H
#define LL_LLCLEANACOUSTICS_H

#include "stdtypes.h"
#include "v3math.h"

struct LLVKWorldFrame;
class LLUUID;

class LLCleanAcoustics
{
public:
    // After buildVulkanWorldFrame: this frame's request (or none).
    static void fillRequest(LLVKWorldFrame& wf);
    // Audio idle, before gAudiop->idle(): enable / disable, harvest, smooth, apply, log.
    static void update();
    // The smoothed listener room while acoustics is active and has measured one: enclosure
    // (side / up hit fraction) and roof (up hit fraction), 0..1. false = no answer (off, no
    // ray tracing, not yet measured). The weather audio's cover (#783, slweatheraudio.h).
    static bool listenerRoom(F32& enclosure, F32& roof);
    // With the room: what the overhead hits are made of, as smoothed shares (sum 1) of the
    // rain-on-roof classes. Starts all wood (the SL default). false = no answer, as above.
    enum ERoofMaterial : U32 { ROOF_WOOD = 0, ROOF_METAL, ROOF_GLASS, ROOF_TENT, ROOF_MAT_COUNT };
    static bool listenerRoofMaterial(F32 shares[ROOF_MAT_COUNT]);
    // #894 OPENINGS around the listener (windows, doors, gaps, open sides) from the room rays:
    // escaped / far directions binned on a fixed sphere, smoothed over ~2 s, grouped into
    // connected openings -- direction (unit, agent axes, outward), solid angle (sr), distance
    // (m, the wall they sit in). Largest first, at most kMaxOpenings; escape = the escaped share
    // of all rays. false = no fresh answer (no ray tracing, or nobody asked). wantOpenings()
    // keeps the room rays coming for ~1 s even with AudioAcoustics off (a room-only request:
    // 64 rays every AudioAcousticsRoomInterval frames); call it each update while wanted.
    struct Opening
    {
        LLVector3 dir;
        F32 solid = 0.f;
        F32 dist = 0.f;
    };
    static constexpr U32 kMaxOpenings = 4;
    static void wantOpenings();
    // #895: nobody wants the openings any more (the weather was reset): the lite room requests
    // stop now instead of after the ~1 s hold, and the last answer is forgotten.
    static void cancelOpenings();
    static bool listenerOpenings(Opening out[kMaxOpenings], U32& n, F32& escape);
    // A sound with no audio channel (#783 thunder: an engine local shot; #892 speakers) traced
    // like a voice: the caller keys it and places it (agent space, within the 200 m source
    // range) at least every half second while it wants an answer; occlusionOf gives its smoothed
    // blocked fraction 0..1 of the straight line (false: off, or not answered yet). Up to 34
    // such sources at a time (keyed; the one expiring first is replaced when full).
    static void traceExtraSource(const LLUUID& key, const LLVector3& pos_agent);
    static bool occlusionOf(const LLUUID& key, F32& occ);
    // Everything the driver applies to a traced source (a channel voice or an extra source),
    // for callers that own their AL source (#892 speakers): direct low-pass, listener-room
    // send, the reverb zone (LLAudioEngine_OpenAL::acousticsZoneSlot; < 0 none) on the second
    // send, and the diffraction pan (place the sound at listener + nlerp(true direction,
    // pan_dir, pan_weight) * true distance). false: off, or not traced / answered yet.
    struct SourceParams
    {
        F32 direct_gain = 1.f, direct_gain_hf = 1.f;
        F32 send_gain = 1.f, send_gain_hf = 1.f;
        S32 zone = -1;
        F32 zone_gain = 0.f, zone_gain_hf = 1.f;
        F32 pan_weight = 0.f;
        LLVector3 pan_dir;   // unit, agent axes
        F32 bass_db = 0.f;   // #897 boundary bass: low-shelf boost (dB) for StreamSourceParams::bassBoostDb
        F32 direct_boost = 1.f;   // #683 early gain: level boost (>= 1) on the source gain, the sends divided by it
                                  // (StreamSourceParams::directBoost); direct_gain stays the filter part (<= 1)
    };
    static bool sourceAcoustics(const LLUUID& key, SourceParams& out);
    // #953: the acoustics' water model runs (AudioAcoustics active + AudioAcousticsUnderwater):
    // it owns what an ear in the water hears (boundary loss, underwater hearing and reverb), so
    // the legacy x5 underwater distance rolloff (llvieweraudio.cpp) must not stack on top.
    static bool ownsUnderwater();
    // (#683 EARLY GAIN, AudioAcousticsEarlyGain: reflected energy arriving from a source within
    // ~50 ms of its direct sound -- traced from the source to the listener -- raises its level,
    // the share the shared reverb does not already play: a sound in a horn is louder in front of
    // the mouth. direct_gain x direct_boost is the level; see "EARLY GAIN" in llcleanacoustics.cpp.)
    // (#897 BOUNDARY BASS: near hard surfaces -- the listener's and the source's -- sounds get a
    // low-shelf boost through an EQ send, +3 dB per close boundary; AudioAcousticsBoundaryBass.)
};

#endif // LL_LLCLEANACOUSTICS_H
