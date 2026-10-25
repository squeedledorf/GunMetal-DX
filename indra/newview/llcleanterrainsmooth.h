/**
 * @file llcleanterrainsmooth.h
 * @brief VK smooth terrain (#211): multi-level meshes of the monotone bicubic surface
 *        (llterrainsmooth.h) through the region height samples, published as the terrain
 *        patch's LOD group levels and kept resident around the camera.
 *
 * DESIGN
 *  - Surface: LLTerrainSmooth::smoothHeight, the same function LLSurface's height /
 *    normal queries use under RenderVKTerrainSmooth (picks, foot planting, parcel lines).
 *    Passes exactly through every sample; normals are its analytic gradient.
 *  - Levels: group level g draws cells of 2^-g m (0: 1 m, 1: 0.5 m, 2: 0.25 m, 3: 0.125 m;
 *    RenderVKTerrainSmoothDetail = the finest level built). Static meshes in the mesh store,
 *    so every view (main, depth pre-pass, shadow cascades, probes, mirror), the per-slot RT
 *    BLAS and the pick boxes consume them with no new pass.
 *  - LOD: the cull picks g from the MAIN camera's distance to the patch box
 *    (clean_terrain_lod.glsl terrainLevel, mirrored by kLevelDist); probe faces draw g 0.
 *    A level not resident falls back coarser first.
 *  - Geomorph: each vertex of level g >= 1 that is not a vertex of level g - 1 stores in
 *    texcoord0.y the height step to where level g - 1's triangle puts it; the VS blends it
 *    in over a distance band ending at the level switch (clean_terrain_lod.glsl
 *    terrainMorph), so a patch is already in its coarser shape when the cull switches it:
 *    no pop. The same blend makes a finer patch's edge meet a coarser neighbour's edge.
 *  - Skirts: every level hangs a two-sided skirt from its four edges, as deep as the
 *    steepest edge cell's rise (+5 cm): any residual crack (a cliff patch whose box is
 *    taller than the LOD band spacing, a level not resident yet, a region border whose
 *    neighbour has not arrived) is bounded by that rise, so the skirt covers it.
 *  - Residency: level 0 (smooth normals, 1 m) for every patch, built at ingest; finer
 *    levels only within kLevelDist[g] + margin of the camera, built nearest first under a
 *    per-frame vertex budget, freed past a wider margin (hysteresis). Content hash of the
 *    patch's samples + one-sample apron + composition: an edit (or a neighbour region
 *    arriving) rebuilds exactly the affected levels with force (the store's identical-
 *    bounds skip would swallow it).
 *  - Off (RenderVKTerrainSmooth = false): every level is the faceted capture again, the
 *    smooth levels are freed, the morph is disabled: today's terrain exactly.
 */
#pragma once

#include "stdtypes.h"

class LLVOSurfacePatch;

namespace LLCleanTerrainSmooth
{
// Camera-to-patch-box distance (m) under which group level g is drawn; [0] unused (the
// rest). MUST match clean_terrain_lod.glsl terrainLevel (and llcleanrtvk.cpp kTerrSwitch).
constexpr F32 kLevelDist[4] = { 0.f, 120.f, 56.f, 12.f };
constexpr S32 kMaxLevel = 3;

bool enabled();   // RenderVKTerrainSmooth
S32  detail();    // RenderVKTerrainSmoothDetail, clamped [0, kMaxLevel]

// From vkIngestTerrainPatch, after the faceted capture. (i0, j0): the patch's lower
// grid corner; comp: the composition at its (gppe + 1)^2 integer grid points (row-major,
// y outer). Smooth off / not
// possible (grid spacing != 1 m): publishes faceted_slot at every level, releases any
// smooth state of the patch, returns false. On: builds / refreshes level 0, publishes
// every resident level, writes the instance box (region-local, every level + skirts)
// to lo / hi and returns true.
bool publishPatch(LLVOSurfacePatch* patchp, U32 faceted_slot, S32 i0, S32 j0, const F32* comp,
                  F32 lo[3], F32 hi[3]);

// Per frame (LLCleanIngest::vkIngestTick): toggle / detail changes, fine-level
// residency around the camera, budgeted builds, dead-patch cleanup.
void tick();
}
