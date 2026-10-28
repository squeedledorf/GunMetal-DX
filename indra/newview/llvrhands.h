/**
 * @file llvrhands.h
 * @brief VR milestone M3 (#1076, epic #1072): visible hands (controller models) and the laser
 * beam, drawn by the GPU-driven world renderer.
 *
 * Each hand is a small stylized controller (two scaled spheres) at the controller's grip
 * pose; the beam is a thin unlit cylinder along the pointing (aim) ray, shown while it points
 * at the UI wall (LLVRInput sets its length). The meshes are captured once into the clean
 * mesh store (procedural LLVolume spheres / cylinders: no third-party assets) and every part
 * is an instance-store row like any prim face: the renderer's instance SSBO + compute cull +
 * indirect draws, no draw call of its own. Rows are re-upserted every frame with the new pose
 * as movers (is_active: the transform does not count as a content change for the ingest's
 * pacing) and carry the dynamic-shadow-caster bit, so the cached shadow tiles never bake them.
 *
 * Desktop (no VR): nothing here runs -- update() is only called from LLVRCamera::apply after
 * the head-tracked camera drove the frame, release() returns at once with nothing registered.
 */
#pragma once

#include "stdtypes.h"

class LLVRHands
{
public:
    // After the head-tracked camera ran (LLVRCamera::apply, main thread): the hand rows at this
    // frame's grip poses, the beams along the aim rays. Untracked hands are released.
    static void update();
    // Drop every row (the session stopped, or the camera left VR). No-op when none exist.
    static void release();
    // The beam of a hand for this frame: its length in metres along the aim ray (<= 0: no
    // beam). Set by LLVRInput before update(); reset by update() after use.
    static void setBeam(U32 hand, F32 length);
};
