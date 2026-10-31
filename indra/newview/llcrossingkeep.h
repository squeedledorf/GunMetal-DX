/**
 * @file llcrossingkeep.h
 * @brief Keep avatars (ours always, others within network draw distance) across region
 *        crossings instead of killing and re-creating them. #607
 *
 * WHAT IS TORN DOWN TODAY. A crossing hands an object from sim A to sim B. Sim B sends a
 * full ObjectUpdate (same UUID, new local id) and sim A sends KillObject for the old local
 * id. The UUID re-key path in LLViewerObjectList::processObjectUpdate (findObject(fullid)
 * -> setUUIDAndLocal -> updateRegion) carries the object over untouched -- but only if
 * B's update arrives FIRST: then A's kill resolves to no object and is ignored. When A's
 * kill wins the race, process_kill_object kills the object: the avatar and every
 * attachment are marked dead (instance-store rows released, LOD-group refs dropped, GPU
 * animation state forgotten, system-body morph streams freed, rig weld / pick state
 * dropped), and B's update then creates them from scratch: Ruthed/clouded avatar, the
 * appearance + bake path, attachments re-rezzed and re-ingested, textures re-requested.
 * Our own avatar is never killed (process_kill_object / killObject skip it), but its
 * attachments and the vehicle we sit on are: BlockAttachmentKillsOnTeleport only covers
 * the teleport state, not crossings.
 *
 * WHAT THIS DOES. A kill that looks like the losing side of that race is DEFERRED instead
 * of applied: the object keeps rendering (LL's region-cross interpolation holds it at the
 * border) and B's update re-keys it (onRekey cancels the pending kill). If no update
 * re-keys it within the grace period, the kill is applied then, exactly as before.
 *  - SELF (our attachments, the vehicle we sit on): deferred when the kill comes from a
 *    region that is not our agent region, or during a crossing / teleport. Long grace.
 *  - OTHER (another avatar, its attachments, an animesh root, a vehicle carrying an
 *    avatar): deferred when its anchor (the avatar / root) already re-keyed to another
 *    region (certain: long grace), or when it is within the horizontal network draw
 *    distance AND near an edge of the killing region with a connected region across it
 *    (heuristic: short grace -- an avatar that logged out / teleported away mid-region is
 *    still killed at once).
 * Pending kills hold no pointers (UUID + the killing (host, local id)), are capped, and
 * OTHER entries are applied at once under RAM (memory governor not comfortable) or VRAM
 * (budget ladder in its reserve) pressure.
 *
 * MEASURE. Per-class deferred / re-keyed / expired / immediate counters, re-key latency,
 * kill->re-create churn (same UUID re-created after a kill), and time-to-fully-rezzed of
 * re-created avatars ("CrossingKeep" log tag, one line in each Crossing568 block).
 *
 * FREEZE WORLD (#1172). While the world is frozen (LLWorldClock) a kill of another avatar,
 * of anything it wears (only along with its wearer's: a plain detach applies), or of a
 * linkset another avatar sits on is HELD, so the frozen picture keeps it (an avatar leaving
 * a private parcel we are in is killed for us by the sim). The thaw applies it, unless the
 * sim sent the object again after the kill (it came back) or it now has another key
 * (re-keyed / re-created); one that is gone by then (its region dropped) is skipped. "Sent
 * again after the kill" needs ordering evidence (slheldkills.h): a live full update the
 * object accepted, from the killing sim, in a packet sent after the kill's. A cache load, an
 * update dropped as older, or one sent before the kill but received after it (reordered or
 * resent) keeps the kill held.
 * A worn object whose wearer is sent again waits for it again, so what the sim does not
 * re-send (re-rezzed under new ids) applies as a detach. The crossing deferral above goes
 * first: a crossing's kill keeps its grace, and one that runs out while frozen is held like
 * any other. Plain objects are not held. Same no-pointer bookkeeping; capped, and applied
 * at once under RAM / VRAM pressure. Not tied to SLCrossingKeep.
 */
#ifndef LL_LLCROSSINGKEEP_H
#define LL_LLCROSSINGKEEP_H

#include "stdtypes.h"
#include <string>

class LLMessageSystem;
class LLViewerObject;
class LLViewerRegion;
class LLVOAvatar;

namespace LLCrossingKeep
{
    // process_kill_object, after the id resolved to a live object: true = the kill was
    // deferred (the caller must not kill it now). sender_ip/port = the killing sim,
    // packet_id = the KillObject's packet on that circuit.
    bool deferKill(LLViewerObject* objectp, LLViewerRegion* sender_region,
                   U32 local_id, U32 sender_ip, U32 sender_port, U32 packet_id);
    // A kill was applied to objectp now (immediate path): churn tracking.
    void noteKilled(LLViewerObject* objectp);
    // processObjectUpdate re-keyed objectp to a new (region, local id): cancels its pending kill.
    void onRekey(LLViewerObject* objectp);
    // LLViewerObjectList::createObject: a UUID killed shortly before came back (churn).
    void onCreated(LLViewerObject* objectp);
    // LLVOAvatar::updateIsFullyLoaded false -> true.
    void onAvatarFullyLoaded(LLVOAvatar* avatarp);
    // Once per frame (main thread): applies expired / pressure-flushed kills.
    void tick();
    // #850 CrossedRegion received: an own-vehicle kill deferred on the edge rule is now a
    // certain crossing (long grace).
    void onCrossedRegion();
    // #850: the last kill of our own vehicle (root) from the agent region within window_s
    // seconds, as "own_vehicle_kill=-2.1s(applied now)" / "(deferred, edge)"; "" if none.
    std::string ownVehicleKillBefore(F64 window_s);
    // #1172 process_kill_object, when deferKill declined the kill: true = the world is frozen
    // and the kill is held until the thaw (the caller must not kill it now).
    bool holdForFreeze(LLViewerObject* objectp, U32 local_id, U32 sender_ip, U32 sender_port, U32 packet_id);
    // #1172 processUpdateCore, after a full (non-terse) update reached objectp: msg = the live
    // message (null: loaded from the cache). If the killing sim sent it after the kill, the
    // held kill is void (and what an avatar wore waits for it again).
    void onFullUpdate(LLViewerObject* objectp, LLMessageSystem* msg);
    // Logout / disconnect: drop everything (the objects die with their regions).
    void clear();

    // One-line counters since the given snapshot (Crossing568 block); snapshot() = now.
    struct Snapshot { U64 v[24] = {}; };
    Snapshot snapshot();
    std::string deltaLine(const Snapshot& since);
    // Live / cap / peak + session totals (stats line / log).
    std::string statsLine();

}

#endif // LL_LLCROSSINGKEEP_H
