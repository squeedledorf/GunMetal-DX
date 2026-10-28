/**
 * @file llvrsafety.h
 * @brief VR milestone M3 (#1076, epic #1072): the frame-rate safety governor.
 *
 * In VR uneven pacing -- frames that miss the display slot the runtime expects -- is what causes
 * motion sickness; a steady lower rate the runtime settled on (SteamVR's 45 Hz motion smoothing)
 * is not punished. While the session runs and you are in the world, the governor watches
 * xrWaitFrame's pacing (LLXrSession::frameTiming: display slots missed against the headset's
 * CURRENT display period) in half-second windows and lowers what costs frame time, draw
 * distance first -- soft caps through the same mechanism as "Keep my FPS" (LLVKPerfCaps, merged
 * in LLVulkanContext, most restrictive wins; settings are never written):
 *  - at once, two levels: a window below half the headset's rate;
 *  - one level: sustained misses -- 2 of the last 3 windows over 10 %, or the last 2 s over 8 %
 *    (a single hitch is not a failure); ignored for 3 s after any level change (its own churn).
 * It restores one level at a time after that level's dwell of clean windows (<= 3 % missed):
 * 10 s, 30 s for the two restores that raise the render scale (70 -> 85 -> 100 %, at levels 5
 * and 3 only, so the frequent 3 <-> 4 moves never toggle it). A restore undone within 30 s
 * doubles that level's dwell (up to 10 min); one that holds 3 min resets it. Every step is
 * logged (tag "VR"). (#1076 headset run 1: the old 1 % / 5 % thresholds with a 1 s settle
 * flapped 3 <-> 4 every minute at a steady 45 Hz, toggling the render scale each time.)
 *
 * SlipstreamVRSafetyGovernor (Preferences > Safety, on by default) turns it off; off restores
 * everything at once.
 *
 * Desktop (no VR): tick() is only called from LLVRInput::update, which returns first.
 */
#pragma once

class LLVRSafety
{
public:
    // Once per frame (LLVRInput::update, VR only). inWorld: the head-tracked world is shown.
    static void tick(bool inWorld);
    // The session stopped: every cap off at once.
    static void reset();
};
