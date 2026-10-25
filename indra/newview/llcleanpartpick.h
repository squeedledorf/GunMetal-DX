/**
 * @file llcleanpartpick.h
 * @brief Picking PARTICLES under Vulkan (#481): the CPU side of the GPU particle pick.
 *
 * LL picked particles against LLVOPartGroup drawables in the PARTITION_PARTICLE octree.
 * The GPU particle system (LLCleanParticleStore + clean_particle_*.comp) keeps every
 * particle in a GPU pool, so that partition is empty and the CPU pick never hit. The
 * renderer now tests the pick segment against every live particle's drawn quad
 * (vkgfx/llcleanpickvk.h, clean_pick_particle.comp) and this module turns its answer
 * into LL's: the hit point and the particle's owner / source object.
 *
 *  - fillRequest: the cursor ray into the frame, ONLY while something recently asked
 *    for a per-frame answer (arm: the world hover; a non-click pick: the debug raycast)
 *    and particles are alive -- otherwise the GPU does no particle pick work at all.
 *  - intersect: the nearest drawn particle on a pick segment, from the per-frame answer
 *    (~2 frames old) when it covers the segment's ray. A click (`sync`: the right
 *    click's pickImmediate, whose menu reads the result at once) whose ray no per-frame
 *    answer covers gets a synchronous GPU pass over the pool the last frame drew.
 *
 * Owner: every particle carries its source's id (LLViewerPartSource mID, baked at emit)
 * and its emitter slot; LLCleanParticleStore keeps slot -> (source id, owner, source
 * object) until the slot is reclaimed, so a particle that outlives its source still
 * names its owner.
 */
#ifndef LL_LLCLEANPARTPICK_H
#define LL_LLCLEANPARTPICK_H

#include "stdtypes.h"
#include "llvector4a.h"
#include "lluuid.h"

struct LLVKWorldFrame;

namespace LLCleanPartPick
{
    // This frame's request (main frame only, after buildVulkanWorldFrame).
    void fillRequest(LLVKWorldFrame& wf);
    // Ask for the per-frame answer over the next few frames (the world hover: a right
    // click then usually finds its answer ready instead of waiting on the GPU).
    void arm();

    struct Hit
    {
        LLVector4a pos;          // agent space
        LLUUID owner;            // the source's owner (LLViewerPartSource::getOwnerUUID)
        LLUUID source;           // the source object (null: no object / unknown)
    };
    // Nearest drawn particle on [start, end] (agent space). `sync`: may wait for a
    // synchronous GPU answer (a click) when no per-frame answer covers the ray;
    // otherwise the per-frame answer only (and asks for one over the next frames).
    bool intersect(const LLVector4a& start, const LLVector4a& end, bool sync, Hit& out);
}

#endif
