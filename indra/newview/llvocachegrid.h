/**
 * @file llvocachegrid.h
 * @brief Flat per-region grid of object-cache root entries, its cull, the look-ahead
 *        bias and the outcome metrics (#479). The object cache's only cull (#480 removed
 *        the VOCache octree it replaced).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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
 */

#ifndef LL_LLVOCACHEGRID_H
#define LL_LLVOCACHEGRID_H

#include "llvector4a.h"
#include "v3math.h"

#include <array>
#include <string>
#include <vector>

class LLCamera;
class LLVOCacheEntry;
class LLViewerRegion;

// A region's object-cache ROOT entries (the entries LLVOCachePartition holds: IN_VO_TREE,
// no parent) binned into 16 x 16 columns of 16 m over the 256 m region, in compact
// per-column arrays: AABB centre (region space) + bin radius, AABB half size, entry.
// Fed by LLVOCachePartition::addEntry / removeEntry (IN_VO_TREE set / cleared),
// plus move() when a root's bounds change while it is held (LLVOCacheEntry keeps a
// back-pointer and its slot; see LLVOCacheEntry::updateParentBoundingInfo).
//
// Add, remove and move are O(1) (swap-remove inside a column). Memory is bounded by the
// region's cache map: the column arrays are high-water buffers, released by clear().
// Each column keeps a conservative AABB of its entries: it grows on add / move and is
// recomputed lazily (on the next query that visits it) after a removal.
class LLVOCacheGrid
{
public:
    static constexpr S32 CELL_METERS = 16;
    static constexpr S32 DIM         = 16;              // 16 x 16 columns over 256 m
    static constexpr S32 NUM_CELLS   = DIM * DIM;

    LLVOCacheGrid();
    ~LLVOCacheGrid();   // detaches every entry (clears their back-pointers)

    LLVOCacheGrid(const LLVOCacheGrid&) = delete;
    LLVOCacheGrid& operator=(const LLVOCacheGrid&) = delete;

    void add(LLVOCacheEntry* entry);
    void remove(LLVOCacheEntry* entry);
    void move(LLVOCacheEntry* entry);       // re-read the entry's bounds (it must be held)
    void clear();
    U32  size() const { return mCount; }

    // FRONT cull: columns rejected against the region frustum (no far plane, camera's
    // region planes must be current: LLCamera::calcRegionFrustumPlanes) and the
    // draw-distance sphere (camera.mFrustumCornerDist, as LL's octree cull); then per
    // entry: frustum + sphere + projection area. Passing entries go to
    // region->addGridVisibleEntry. Returns the number selected.
    U32 selectFront(LLCamera& camera, const LLVector3& local_origin, F32 pixel_threshold,
                    F32 near_radius, LLViewerRegion* region);
    // SPHERE query (the rear sphere, and the look-ahead sphere): entries whose AABB meets
    // the sphere and whose projection from local_origin passes. Returns the number selected.
    U32 selectSphere(const LLVector3& local_origin, F32 radius, F32 pixel_threshold,
                     F32 near_radius, LLViewerRegion* region);

    // Per-entry rule shared by the cull, the metrics and the overlay: LL's
    // checkProjectionArea formula on the entry's bin radius -- always true within
    // near_radius, else bin_radius^2 / (distance - near_radius) > threshold. (LL's octree
    // passed object-extents min/max into checkProjectionArea's centre/size, a near no-op;
    // the bin radius is what calcSceneContribution gates on afterwards.)
    static bool passesProjection(const LLVector4a& center_bin, const LLVector4a& origin,
                                 F32 pixel_threshold, F32 near_radius);
    // Squared distance from a point to an AABB (centre / half size).
    static F32 distSquaredToBox(const LLVector4a& center, const LLVector4a& half, const LLVector4a& point);

    // Visit cells [first, first + count) (row-major, wraps): fn(entry, centre_bin, half).
    template <typename F> void forEachInCells(S32 first, S32 count, F&& fn)
    {
        for (S32 i = 0; i < count; ++i)
        {
            Cell& cell = mCells[(first + i) % NUM_CELLS];
            for (size_t k = 0; k < cell.mEntries.size(); ++k)
            {
                fn(cell.mEntries[k], cell.mCenter[k], cell.mHalf[k]);
            }
        }
    }
    // Visit entries whose AABB meets the sphere (region space): fn(entry, centre_bin, half).
    template <typename F> void forEachNear(const LLVector3& local_origin, F32 radius, F&& fn)
    {
        LLVector4a origin;
        origin.load3(local_origin.mV);
        const F32 r2 = radius * radius;
        for (S32 c = 0; c < NUM_CELLS; ++c)
        {
            Cell& cell = mCells[c];
            if (cell.mEntries.empty() || !cellMeetsSphere(cell, origin, r2))
            {
                continue;
            }
            for (size_t k = 0; k < cell.mEntries.size(); ++k)
            {
                if (distSquaredToBox(cell.mCenter[k], cell.mHalf[k], origin) <= r2)
                {
                    fn(cell.mEntries[k], cell.mCenter[k], cell.mHalf[k]);
                }
            }
        }
    }

private:
    struct Cell
    {
        std::vector<LLVector4a>      mCenter;    // xyz AABB centre (region space), w bin radius
        std::vector<LLVector4a>      mHalf;      // xyz AABB half size, w 0
        std::vector<LLVOCacheEntry*> mEntries;
        LLVector4a                   mMin, mMax; // conservative bounds of the column's AABBs
        bool                         mBoundsDirty = false;
    };

    static S32  cellOf(const LLVector4a& center);
    static bool readBounds(LLVOCacheEntry* entry, LLVector4a& center_bin, LLVector4a& half);
    static void growBounds(Cell& cell, const LLVector4a& center, const LLVector4a& half);
    static void refreshBounds(Cell& cell);
    static bool cellMeetsSphere(Cell& cell, const LLVector4a& origin, F32 radius_squared);
    void insertInto(S32 cell_idx, LLVOCacheEntry* entry, const LLVector4a& center_bin, const LLVector4a& half);
    void eraseFrom(LLVOCacheEntry* entry);

    std::array<Cell, NUM_CELLS> mCells;
    U32 mCount = 0;
};

// Look-ahead (#479, RenderVKVOCacheLookAhead, default off): while the culling camera
// moves fast, the grid front cull also selects around the point the camera reaches in
// RenderVKVOCacheLookAheadSeconds, and updateVisibleEntries ranks entries by the larger
// of their contribution from the camera and from that point. Selection and priority
// only; nothing else reads it.
namespace LLVOCacheLookAhead
{
    // Once per frame before the cache cull: velocity from the culling camera's motion
    // (covers walking, flying, vehicles and flycam), smoothed; inactive while frozen.
    void update();
    bool isActive();
    const LLVector3& getPointAgent();   // agent space
    F32  getRadius();
}

// Outcome metrics (#479, RenderVKVOCacheMetrics, default off), logged every 5 s under tag
// VOCacheMetrics:
//   pop-in latency (sampled "on screen + above the pixel threshold" -> instantiated,
//   median / p90), missing-on-screen count, cache cull CPU time, objects created from
//   the cache per second, waiting-list size; plus the light invariant check (eligible
//   root entries the last front cull did not select, without a reason).
namespace LLVOCacheMetrics
{
    bool enabled();
    void beginFrame();                      // reads the setting (driver, before the culls)
    void addCullTime(F64 seconds);          // per region cull call
    void onCreatedFromCache();              // LLViewerRegion::createVisibleObjects
    void onActivated(LLVOCacheEntry* entry);// LLViewerRegion::addActiveCacheEntry
    void onWaitingList(U32 size);           // after LLViewerRegion::updateVisibleEntries
    void tick();                            // driver, after the culls: sample + log
}

// Frozen-camera overlay (#479): the frozen frustum, the draw-distance / rear / look-ahead
// spheres and wire boxes for cache entries near the frozen camera coloured by state.
// Drawn from the VK world-overlay segment while the culling camera is frozen.
namespace LLVOCacheDebug
{
    void renderOverlay();
    const std::vector<std::string>& legend();   // LLDebugText lines (empty when not frozen)
}

#endif // LL_LLVOCACHEGRID_H
