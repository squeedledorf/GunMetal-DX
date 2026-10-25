/**
 * @file llvocachegrid.cpp
 * @brief Flat per-region grid of object-cache root entries, its cull, the look-ahead
 *        bias, the outcome metrics and the frozen-camera overlay (#479).
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

#include "llviewerprecompiledheaders.h"
#include "llvocachegrid.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcullcamera.h"
#include "lldrawable.h"
#include "llgl.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llworld.h"

#include <algorithm>
#include <cmath>

//---------------------------------------------------------------------------
// LLVOCacheGrid
//---------------------------------------------------------------------------

LLVOCacheGrid::LLVOCacheGrid()
{
    for (Cell& cell : mCells)
    {
        cell.mMin.clear();
        cell.mMax.clear();
    }
}

LLVOCacheGrid::~LLVOCacheGrid()
{
    clear();
}

void LLVOCacheGrid::clear()
{
    for (Cell& cell : mCells)
    {
        for (LLVOCacheEntry* entry : cell.mEntries)
        {
            entry->mGrid = nullptr;
            entry->mGridCell = -1;
            entry->mGridIndex = 0;
        }
        // Release the high-water buffers.
        std::vector<LLVector4a>().swap(cell.mCenter);
        std::vector<LLVector4a>().swap(cell.mHalf);
        std::vector<LLVOCacheEntry*>().swap(cell.mEntries);
        cell.mMin.clear();
        cell.mMax.clear();
        cell.mBoundsDirty = false;
    }
    mCount = 0;
}

//static
S32 LLVOCacheGrid::cellOf(const LLVector4a& center)
{
    // Centres can sit slightly outside [0, 256) (linksets over the edge): clamp.
    const S32 x = llclamp((S32)std::floor(center[0] / (F32)CELL_METERS), 0, DIM - 1);
    const S32 y = llclamp((S32)std::floor(center[1] / (F32)CELL_METERS), 0, DIM - 1);
    return y * DIM + x;
}

//static
bool LLVOCacheGrid::readBounds(LLVOCacheEntry* entry, LLVector4a& center_bin, LLVector4a& half)
{
    if (!entry || !entry->getEntry())
    {
        return false;
    }
    // Centre = the entry's group position (what calcSceneContribution measures from),
    // half size = a box around it that contains the spatial extents (conservative).
    const LLVector4a& pos = entry->getPositionGroup();
    const LLVector4a* ext = entry->getSpatialExtents();
    LLVector4a lo, hi;
    lo.setSub(pos, ext[0]);
    lo.setAbs(lo);
    hi.setSub(ext[1], pos);
    hi.setAbs(hi);
    half.setMax(lo, hi);
    center_bin = pos;
    const F32 bin_radius = entry->getBinRadius();
    if (!center_bin.isFinite3() || !half.isFinite3() || !llfinite(bin_radius))
    {
        return false;
    }
    center_bin.getF32ptr()[3] = bin_radius;
    half.getF32ptr()[3] = 0.f;
    return true;
}

//static
void LLVOCacheGrid::growBounds(Cell& cell, const LLVector4a& center, const LLVector4a& half)
{
    LLVector4a lo, hi;
    lo.setSub(center, half);
    hi.setAdd(center, half);
    if (cell.mEntries.size() == 1)
    {
        cell.mMin = lo;
        cell.mMax = hi;
        cell.mBoundsDirty = false;
    }
    else
    {
        cell.mMin.setMin(cell.mMin, lo);
        cell.mMax.setMax(cell.mMax, hi);
    }
}

//static
void LLVOCacheGrid::refreshBounds(Cell& cell)
{
    if (!cell.mBoundsDirty)
    {
        return;
    }
    cell.mBoundsDirty = false;
    if (cell.mEntries.empty())
    {
        cell.mMin.clear();
        cell.mMax.clear();
        return;
    }
    LLVector4a lo, hi;
    for (size_t k = 0; k < cell.mEntries.size(); ++k)
    {
        lo.setSub(cell.mCenter[k], cell.mHalf[k]);
        hi.setAdd(cell.mCenter[k], cell.mHalf[k]);
        if (k == 0)
        {
            cell.mMin = lo;
            cell.mMax = hi;
        }
        else
        {
            cell.mMin.setMin(cell.mMin, lo);
            cell.mMax.setMax(cell.mMax, hi);
        }
    }
}

//static
F32 LLVOCacheGrid::distSquaredToBox(const LLVector4a& center, const LLVector4a& half, const LLVector4a& point)
{
    LLVector4a d;
    d.setSub(point, center);
    d.setAbs(d);
    d.sub(half);
    LLVector4a zero;
    zero.clear();
    d.setMax(d, zero);
    return d.dot3(d).getF32();
}

//static
bool LLVOCacheGrid::cellMeetsSphere(Cell& cell, const LLVector4a& origin, F32 radius_squared)
{
    refreshBounds(cell);
    LLVector4a center, half;
    center.setAdd(cell.mMin, cell.mMax);
    center.mul(0.5f);
    half.setSub(cell.mMax, cell.mMin);
    half.mul(0.5f);
    return distSquaredToBox(center, half, origin) <= radius_squared;
}

//static
bool LLVOCacheGrid::passesProjection(const LLVector4a& center_bin, const LLVector4a& origin,
                                     F32 pixel_threshold, F32 near_radius)
{
    LLVector4a look_at;
    look_at.setSub(center_bin, origin);
    F32 distance = look_at.getLength3().getF32();
    if (distance <= near_radius)
    {
        return true;   // always load close-by objects
    }
    distance -= near_radius;
    const F32 rad = center_bin[3];
    return rad * rad / distance > pixel_threshold;
}

void LLVOCacheGrid::insertInto(S32 cell_idx, LLVOCacheEntry* entry, const LLVector4a& center_bin, const LLVector4a& half)
{
    Cell& cell = mCells[cell_idx];
    cell.mCenter.push_back(center_bin);
    cell.mHalf.push_back(half);
    cell.mEntries.push_back(entry);
    entry->mGrid = this;
    entry->mGridCell = cell_idx;
    entry->mGridIndex = (U32)(cell.mEntries.size() - 1);
    growBounds(cell, center_bin, half);
    ++mCount;
}

void LLVOCacheGrid::eraseFrom(LLVOCacheEntry* entry)
{
    const S32 cell_idx = entry->mGridCell;
    if (cell_idx < 0 || cell_idx >= NUM_CELLS)
    {
        return;
    }
    Cell& cell = mCells[cell_idx];
    const U32 idx = entry->mGridIndex;
    if (idx >= cell.mEntries.size() || cell.mEntries[idx] != entry)
    {
        llassert(false);   // back-pointer out of sync: never expected
        return;
    }
    const U32 last = (U32)(cell.mEntries.size() - 1);
    if (idx != last)
    {
        cell.mCenter[idx] = cell.mCenter[last];
        cell.mHalf[idx] = cell.mHalf[last];
        cell.mEntries[idx] = cell.mEntries[last];
        cell.mEntries[idx]->mGridIndex = idx;
    }
    cell.mCenter.pop_back();
    cell.mHalf.pop_back();
    cell.mEntries.pop_back();
    cell.mBoundsDirty = true;   // can't shrink in O(1): recomputed on the next visit
    entry->mGrid = nullptr;
    entry->mGridCell = -1;
    entry->mGridIndex = 0;
    --mCount;
}

void LLVOCacheGrid::add(LLVOCacheEntry* entry)
{
    if (!entry)
    {
        return;
    }
    if (entry->mGrid == this)
    {
        move(entry);
        return;
    }
    if (entry->mGrid)
    {
        entry->mGrid->remove(entry);   // (never expected: one region per entry)
    }
    LLVector4a center_bin, half;
    if (!readBounds(entry, center_bin, half))
    {
        return;
    }
    insertInto(cellOf(center_bin), entry, center_bin, half);
}

void LLVOCacheGrid::remove(LLVOCacheEntry* entry)
{
    if (!entry || entry->mGrid != this)
    {
        return;
    }
    eraseFrom(entry);
}

void LLVOCacheGrid::move(LLVOCacheEntry* entry)
{
    if (!entry || entry->mGrid != this)
    {
        return;
    }
    LLVector4a center_bin, half;
    if (!readBounds(entry, center_bin, half))
    {
        eraseFrom(entry);
        return;
    }
    const S32 cell_idx = cellOf(center_bin);
    if (cell_idx == entry->mGridCell)
    {
        Cell& cell = mCells[cell_idx];
        cell.mCenter[entry->mGridIndex] = center_bin;
        cell.mHalf[entry->mGridIndex] = half;
        growBounds(cell, center_bin, half);
        cell.mBoundsDirty = true;   // it may also have shrunk: tighten on the next visit
        return;
    }
    eraseFrom(entry);
    insertInto(cell_idx, entry, center_bin, half);
}

U32 LLVOCacheGrid::selectFront(LLCamera& camera, const LLVector3& local_origin, F32 pixel_threshold,
                               F32 near_radius, LLViewerRegion* region)
{
    LLVector4a origin;
    origin.load3(local_origin.mV);
    // The draw-distance sphere: the far frustum corner distance (as LL's VOCache octree
    // cull used, before #480 removed it).
    const F32 sphere_r2 = camera.mFrustumCornerDist * camera.mFrustumCornerDist;
    U32 selected = 0;
    LLVector4a cell_center, cell_half;
    for (S32 c = 0; c < NUM_CELLS; ++c)
    {
        Cell& cell = mCells[c];
        if (cell.mEntries.empty())
        {
            continue;
        }
        // Column reject: frustum (no far plane) ∩ draw-distance sphere.
        refreshBounds(cell);
        cell_center.setAdd(cell.mMin, cell.mMax);
        cell_center.mul(0.5f);
        cell_half.setSub(cell.mMax, cell.mMin);
        cell_half.mul(0.5f);
        const S32 frustum_res = camera.AABBInRegionFrustumNoFarClip(cell_center, cell_half);
        if (frustum_res == 0 || distSquaredToBox(cell_center, cell_half, origin) > sphere_r2)
        {
            continue;
        }
        // Per entry: frustum (skipped when the whole column is inside) + sphere + projection.
        const size_t n = cell.mEntries.size();
        for (size_t k = 0; k < n; ++k)
        {
            const LLVector4a& center = cell.mCenter[k];
            const LLVector4a& half = cell.mHalf[k];
            if (frustum_res != 2 && !camera.AABBInRegionFrustumNoFarClip(center, half))
            {
                continue;
            }
            if (distSquaredToBox(center, half, origin) > sphere_r2)
            {
                continue;
            }
            if (!passesProjection(center, origin, pixel_threshold, near_radius))
            {
                continue;
            }
            region->addGridVisibleEntry(cell.mEntries[k]);
            ++selected;
        }
    }
    return selected;
}

U32 LLVOCacheGrid::selectSphere(const LLVector3& local_origin, F32 radius, F32 pixel_threshold,
                                F32 near_radius, LLViewerRegion* region)
{
    LLVector4a origin;
    origin.load3(local_origin.mV);
    U32 selected = 0;
    forEachNear(local_origin, radius,
        [&](LLVOCacheEntry* entry, const LLVector4a& center, const LLVector4a& /*half*/)
        {
            if (passesProjection(center, origin, pixel_threshold, near_radius))
            {
                region->addGridVisibleEntry(entry);
                ++selected;
            }
        });
    return selected;
}

//---------------------------------------------------------------------------
// LLVOCacheLookAhead
//---------------------------------------------------------------------------

namespace
{
    bool        sLookActive = false;
    LLVector3   sLookPoint;
    F32         sLookRadius = 0.f;
    // Culling-camera velocity (global, smoothed).
    bool        sHaveLastPos = false;
    LLVector3d  sLastPosGlobal;
    F64         sLastPosTime = 0.0;
    LLVector3   sCamVelocity;
}

namespace LLVOCacheLookAhead
{

void update()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKVOCacheLookAhead", false);
    static LLCachedControl<F32>  s_seconds(gSavedSettings, "RenderVKVOCacheLookAheadSeconds", 1.5f);
    static LLCachedControl<F32>  s_min_speed(gSavedSettings, "RenderVKVOCacheLookAheadMinSpeed", 10.f);

    sLookActive = false;

    // Velocity from the culling camera's own motion: covers walking, flying, vehicles
    // (the seated avatar's velocity reads ~0) and flycam alike.
    const F64 now = LLFrameTimer::getElapsedSeconds();
    const LLVector3 cam_agent = LLCullCamera::getOrigin();
    const LLVector3d pos_global = gAgent.getPosGlobalFromAgent(cam_agent);
    if (sHaveLastPos)
    {
        const F64 dt = now - sLastPosTime;
        if (dt > 0.0005)
        {
            const LLVector3 v((pos_global - sLastPosGlobal) * (1.0 / dt));
            if (dt > 0.5 || v.lengthSquared() > 1000.f * 1000.f)
            {
                sCamVelocity.clear();   // a stall, teleport or camera cut: no velocity
            }
            else
            {
                const F32 blend = llmin((F32)(dt / 0.25), 1.f);   // ~0.25 s smoothing
                sCamVelocity += (v - sCamVelocity) * blend;
            }
        }
        else
        {
            return;   // same instant: keep the previous state
        }
    }
    sLastPosGlobal = pos_global;
    sLastPosTime = now;
    sHaveLastPos = true;

    if (!s_on || LLCullCamera::isFrozen()
        || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        return;
    }
    const F32 speed = sCamVelocity.length();
    if (speed < llmax((F32)s_min_speed, 0.5f))
    {
        return;
    }
    const F32 draw = llmax(gAgentCamera.mDrawDistance, 32.f);
    const F32 ahead = llmin(speed * llclamp((F32)s_seconds, 0.1f, 5.f), draw);
    sLookPoint = cam_agent + sCamVelocity * (ahead / speed);
    sLookRadius = llclamp(ahead * 0.5f, 32.f, draw);
    sLookActive = true;
}

bool isActive()
{
    return sLookActive;
}

const LLVector3& getPointAgent()
{
    return sLookPoint;
}

F32 getRadius()
{
    return sLookRadius;
}

} // namespace LLVOCacheLookAhead

//---------------------------------------------------------------------------
// LLVOCacheMetrics
//---------------------------------------------------------------------------

namespace
{
    constexpr F64 kLogPeriod   = 5.0;    // s
    constexpr S32 kSliceCells  = 64;     // cells per region per tick: a full sweep every 4 frames
    constexpr U32 kLatencyCap  = 2048;   // latency samples kept per window (ring)
    constexpr U32 kLogUnexplained = 5;   // unexplained entries logged per window
    constexpr F64 kStuckSeconds = 2.0;

    bool sMetricsOn = false;
    F64  sWindowStart = 0.0;

    F64  sCullFrameAcc = 0.0, sCullSum = 0.0, sCullMax = 0.0;
    U32  sCullFrames = 0;
    U32  sCreated = 0;
    U64  sWaitSum = 0;
    U32  sWaitSamples = 0, sWaitMax = 0;

    F32  sLatency[kLatencyCap];
    F32  sLatencyScratch[kLatencyCap];
    U32  sLatencyCount = 0;     // valid samples (<= cap)
    U32  sLatencyTotal = 0;     // samples seen in the window (ring position)
    F32  sLatencyMax = 0.f;

    S32  sSweepCell = 0;
    U32  sSweepMissing = 0, sSweepStuck = 0;
    U32  sLastMissing = 0, sLastStuck = 0, sMissingMax = 0;

    U32  sInvChecked = 0, sInvUnexplained = 0, sInvHeld = 0, sInvBadSelected = 0, sInvLogged = 0;

    void resetWindow(F64 now)
    {
        sWindowStart = now;
        sCullSum = sCullMax = 0.0;
        sCullFrames = 0;
        sCreated = 0;
        sWaitSum = 0;
        sWaitSamples = sWaitMax = 0;
        sLatencyCount = sLatencyTotal = 0;
        sLatencyMax = 0.f;
        sMissingMax = 0;
        sInvChecked = sInvUnexplained = sInvHeld = sInvBadSelected = sInvLogged = 0;
    }

    F32 percentile(U32 n, F32 p)
    {
        if (!n)
        {
            return 0.f;
        }
        U32 k = (U32)(p * (F32)(n - 1) + 0.5f);
        k = llmin(k, n - 1);
        std::nth_element(sLatencyScratch, sLatencyScratch + k, sLatencyScratch + n);
        return sLatencyScratch[k];
    }
}

namespace LLVOCacheMetrics
{

bool enabled()
{
    return sMetricsOn;
}

void beginFrame()
{
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKVOCacheMetrics", false);
    const bool on = s_on;
    if (on && !sMetricsOn)
    {
        resetWindow(LLFrameTimer::getElapsedSeconds());
        sSweepCell = 0;
        sSweepMissing = sSweepStuck = sLastMissing = sLastStuck = 0;
    }
    sMetricsOn = on;
    sCullFrameAcc = 0.0;
}

void addCullTime(F64 seconds)
{
    sCullFrameAcc += seconds;
}

void onCreatedFromCache()
{
    if (sMetricsOn)
    {
        ++sCreated;
    }
}

void onActivated(LLVOCacheEntry* entry)
{
    if (!entry)
    {
        return;
    }
    const F64 since = entry->mEligibleSince;
    entry->mEligibleSince = 0.0;
    if (!sMetricsOn || since <= 0.0)
    {
        return;
    }
    const F32 ms = (F32)((LLFrameTimer::getElapsedSeconds() - since) * 1000.0);
    sLatency[sLatencyTotal % kLatencyCap] = ms;
    ++sLatencyTotal;
    sLatencyCount = llmin(sLatencyTotal, kLatencyCap);
    sLatencyMax = llmax(sLatencyMax, ms);
}

void onWaitingList(U32 size)
{
    if (sMetricsOn)
    {
        sWaitSum += size;
        ++sWaitSamples;
        sWaitMax = llmax(sWaitMax, size);
    }
}

void tick()
{
    if (!sMetricsOn)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();

    // Cull CPU time, per frame (all regions).
    sCullSum += sCullFrameAcc;
    sCullMax = llmax(sCullMax, sCullFrameAcc);
    ++sCullFrames;

    // ---- Sampled sweep: a slice of every region's columns per tick. "Eligible" = the
    // front rules from the culling camera (frustum without far plane, draw-distance
    // sphere, projection) AND the scene contribution updateVisibleEntries gates on
    // (draw distance + bin radius, pixel threshold): on screen and above threshold.
    // Every eligible grid entry is missing on screen (instantiated roots leave the grid).
    LLCamera cam(LLCullCamera::get());   // copy: the region planes are rebuilt per region
    const LLVector3 cam_agent = cam.getOrigin();
    const F32 threshold = LLVOCacheEntry::getSquaredPixelThreshold(true);
    const F32 near_radius = LLVOCacheEntry::sNearRadius;
    const F32 draw = gAgentCamera.mDrawDistance;
    const F32 sphere_r2 = cam.mFrustumCornerDist * cam.mFrustumCornerDist;
    const U32 frame = (U32)LLSpatialEntryData::getCurrentFrame();
    const bool look_ahead = LLVOCacheLookAhead::isActive();

    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        LLVOCachePartition* part = region ? region->getVOCachePartition() : nullptr;
        if (!part || region->isPaused())
        {
            continue;
        }
        const LLVector3 region_agent = region->getOriginAgent();
        cam.calcRegionFrustumPlanes(region_agent, draw);
        LLVector4a origin;
        origin.load3((cam_agent - region_agent).mV);
        // The invariant check only on frames this region's front cull actually ran
        // (otherwise the static-camera throttle is the reason).
        const bool front_now = part->frontCulledOnFrame(frame);

        part->getGrid().forEachInCells(sSweepCell, kSliceCells,
            [&](LLVOCacheEntry* entry, const LLVector4a& center, const LLVector4a& half)
            {
                if (!entry->isValid())
                {
                    entry->mEligibleSince = 0.0;   // reason: invalid
                    return;
                }
                const bool cull_pass = cam.AABBInRegionFrustumNoFarClip(center, half)
                    && LLVOCacheGrid::distSquaredToBox(center, half, origin) <= sphere_r2
                    && LLVOCacheGrid::passesProjection(center, origin, threshold, near_radius);
                const bool selected = entry->mSelectedFrame == frame;
                if (front_now && !look_ahead && selected && !cull_pass)
                {
                    ++sInvBadSelected;   // selected by the grid front cull yet fails its rules
                }
                if (!cull_pass || entry->sceneContributionFrom(origin, draw) <= threshold)
                {
                    entry->mEligibleSince = 0.0;   // off screen / below threshold
                    return;
                }
                if (entry->isVramHeld())
                {
                    entry->mEligibleSince = 0.0;   // reason: VRAM hold (#405)
                    if (front_now)
                    {
                        ++sInvHeld;
                    }
                    return;
                }
                if (entry->mEligibleSince <= 0.0)
                {
                    entry->mEligibleSince = now;
                }
                ++sSweepMissing;
                if (now - entry->mEligibleSince > kStuckSeconds)
                {
                    ++sSweepStuck;
                }
                if (front_now)
                {
                    ++sInvChecked;
                    if (!selected)
                    {
                        ++sInvUnexplained;
                        if (sInvLogged < kLogUnexplained)
                        {
                            ++sInvLogged;
                            LLVector4a d;
                            d.setSub(center, origin);
                            LL_INFOS("VOCacheMetrics") << "invariant: eligible root not selected, no reason: local_id "
                                << entry->getLocalID() << " region " << region->getName()
                                << " pos " << center[0] << "," << center[1] << "," << center[2]
                                << " bin_radius " << center[3] << " dist " << d.getLength3().getF32()
                                << " state " << entry->getState()
                                << LL_ENDL;
                        }
                    }
                }
            });
    }
    sSweepCell += kSliceCells;
    if (sSweepCell >= LLVOCacheGrid::NUM_CELLS)
    {
        sSweepCell = 0;
        sLastMissing = sSweepMissing;
        sLastStuck = sSweepStuck;
        sMissingMax = llmax(sMissingMax, sSweepMissing);
        sSweepMissing = sSweepStuck = 0;
    }

    // ---- Summary line.
    const F64 window = now - sWindowStart;
    if (window < kLogPeriod)
    {
        return;
    }
    const U32 n = sLatencyCount;
    std::copy(sLatency, sLatency + n, sLatencyScratch);
    const F32 med = percentile(n, 0.5f);
    const F32 p90 = percentile(n, 0.9f);
    U32 grid_entries = 0;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        if (LLVOCachePartition* part = region ? region->getVOCachePartition() : nullptr)
        {
            grid_entries += part->getGrid().size();
        }
    }
    LL_INFOS("VOCacheMetrics") << "lookahead=" << (look_ahead ? "active" : (gSavedSettings.getBOOL("RenderVKVOCacheLookAhead") ? "idle" : "off"))
        << " frozen=" << (LLCullCamera::isFrozen() ? 1 : 0)
        << llformat(" | popin n=%u med=%.0fms p90=%.0fms max=%.0fms", sLatencyTotal, med, p90, sLatencyMax)
        << llformat(" | missing=%u max=%u stuck>2s=%u", sLastMissing, sMissingMax, sLastStuck)
        << llformat(" | cull avg=%.3fms max=%.3fms", sCullFrames ? sCullSum * 1000.0 / sCullFrames : 0.0, sCullMax * 1000.0)
        << llformat(" | created/s=%.1f", (F64)sCreated / window)
        << llformat(" | waiting avg=%.0f max=%u", sWaitSamples ? (F64)sWaitSum / sWaitSamples : 0.0, sWaitMax)
        << llformat(" | invariant checked=%u unexplained=%u held=%u bad_selected=%u", sInvChecked, sInvUnexplained, sInvHeld, sInvBadSelected)
        << " | grid_entries=" << grid_entries
        << LL_ENDL;
    resetWindow(now);
}

} // namespace LLVOCacheMetrics

//---------------------------------------------------------------------------
// LLVOCacheDebug: frozen-camera overlay
//---------------------------------------------------------------------------

namespace
{
    std::vector<std::string> sLegend;

    void wireBox(const LLVector4a& center, const LLVector4a& half, const LLVector3& offset)
    {
        const LLVector3 c(center[0] + offset.mV[0], center[1] + offset.mV[1], center[2] + offset.mV[2]);
        const LLVector3 h(half[0], half[1], half[2]);
        LLVector3 v[8];
        for (S32 i = 0; i < 8; ++i)
        {
            v[i].set(c.mV[0] + ((i & 1) ? h.mV[0] : -h.mV[0]),
                     c.mV[1] + ((i & 2) ? h.mV[1] : -h.mV[1]),
                     c.mV[2] + ((i & 4) ? h.mV[2] : -h.mV[2]));
        }
        static const U8 edges[12][2] = { {0,1},{2,3},{4,5},{6,7}, {0,2},{1,3},{4,6},{5,7}, {0,4},{1,5},{2,6},{3,7} };
        for (const auto& e : edges)
        {
            gGL.vertex3fv(v[e[0]].mV);
            gGL.vertex3fv(v[e[1]].mV);
        }
    }

    // Three great circles (XY, XZ, YZ) of a sphere, agent space.
    void wireSphere(const LLVector3& c, F32 r)
    {
        constexpr S32 SEG = 64;
        for (S32 plane = 0; plane < 3; ++plane)
        {
            for (S32 i = 0; i < SEG; ++i)
            {
                const F32 a0 = F_TWO_PI * (F32)i / SEG;
                const F32 a1 = F_TWO_PI * (F32)(i + 1) / SEG;
                const F32 c0 = cosf(a0) * r, s0 = sinf(a0) * r;
                const F32 c1 = cosf(a1) * r, s1 = sinf(a1) * r;
                LLVector3 p0, p1;
                if (plane == 0)      { p0.set(c0, s0, 0.f); p1.set(c1, s1, 0.f); }
                else if (plane == 1) { p0.set(c0, 0.f, s0); p1.set(c1, 0.f, s1); }
                else                 { p0.set(0.f, c0, s0); p1.set(0.f, c1, s1); }
                p0 += c;
                p1 += c;
                gGL.vertex3fv(p0.mV);
                gGL.vertex3fv(p1.mV);
            }
        }
    }
}

namespace LLVOCacheDebug
{

void renderOverlay()
{
    if (!LLCullCamera::isFrozen())
    {
        sLegend.clear();
        return;
    }
    static LLCachedControl<F32> s_radius(gSavedSettings, "RenderVKVOCacheOverlayRadius", 128.f);
    static LLCachedControl<U32> s_max_boxes(gSavedSettings, "RenderVKVOCacheOverlayMaxBoxes", 4000);
    const F32 radius = llclamp((F32)s_radius, 8.f, 1024.f);
    const U32 max_boxes = llclamp((U32)s_max_boxes, 0u, 50000u);

    LLCamera& cam = LLCullCamera::get();
    const LLVector3 origin = cam.getOrigin();
    const U32 frame = (U32)LLSpatialEntryData::getCurrentFrame();
    // "Recently selected": the static-camera throttle runs the front cull only every
    // few frames (always, while frozen), so a single-frame stamp would flicker.
    constexpr U32 kRecentFrames = 64;
    const F32 threshold = LLVOCacheEntry::getSquaredPixelThreshold(true);

    LLRender::UIScope gls_ui;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    // Frustum + spheres: through geometry.
    {
        LLRender::DepthScope depth(false, false);
        gGL.begin(LLRender::LINES);
        gGL.color4f(1.f, 1.f, 1.f, 1.f);   // frozen frustum (far corners at the sphere radius)
        const LLVector3* f = cam.mAgentFrustum;
        static const U8 fedges[12][2] = { {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7} };
        for (const auto& e : fedges)
        {
            gGL.vertex3fv(f[e[0]].mV);
            gGL.vertex3fv(f[e[1]].mV);
        }
        for (S32 i = 0; i < 4; ++i)   // apex rays
        {
            gGL.vertex3fv(origin.mV);
            gGL.vertex3fv(f[i].mV);
        }
        gGL.color4f(0.f, 0.9f, 0.9f, 0.6f);   // draw-distance sphere (frustum corner distance)
        wireSphere(origin, cam.mFrustumCornerDist);
        gGL.color4f(0.3f, 0.4f, 1.f, 0.8f);   // rear sphere (sRearFarRadius)
        wireSphere(origin, LLVOCacheEntry::sRearFarRadius);
        gGL.end();
    }

    // Entry boxes, depth-tested, within radius of the frozen camera.
    U32 n_active = 0, n_waiting = 0, n_below = 0, n_unselected = 0, n_held = 0, drawn = 0;
    {
        LLRender::DepthScope depth(true, false);
        gGL.begin(LLRender::LINES);
        const F32 r2 = radius * radius;
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            LLVOCachePartition* part = region ? region->getVOCachePartition() : nullptr;
            if (!part)
            {
                continue;
            }
            const LLVector3 region_agent = region->getOriginAgent();
            // Cached (not instantiated) roots, from the grid.
            part->getGrid().forEachNear(origin - region_agent, radius,
                [&](LLVOCacheEntry* entry, const LLVector4a& center, const LLVector4a& half)
                {
                    if (drawn >= max_boxes || !entry->isValid())
                    {
                        return;
                    }
                    if (entry->isVramHeld())
                    {
                        gGL.color4f(1.f, 0.f, 1.f, 1.f);   // VRAM held (#405)
                        ++n_held;
                    }
                    else if (entry->getState() == LLVOCacheEntry::IN_QUEUE || entry->getState() == LLVOCacheEntry::WAITING
                             || (frame - entry->mSelectedFrame < kRecentFrames && entry->getSceneContribution() > threshold))
                    {
                        gGL.color4f(1.f, 0.9f, 0.f, 1.f);  // selected, waiting to be instantiated
                        ++n_waiting;
                    }
                    else if (frame - entry->mSelectedFrame < kRecentFrames)
                    {
                        gGL.color4f(1.f, 0.5f, 0.f, 0.9f); // selected, below the scene-contribution threshold
                        ++n_below;
                    }
                    else
                    {
                        gGL.color4f(0.55f, 0.55f, 0.55f, 0.6f); // not selected
                        ++n_unselected;
                    }
                    wireBox(center, half, region_agent);
                    ++drawn;
                });
            // Instantiated roots (they leave the grid): the drawable's extents, agent space.
            gGL.color4f(0.f, 1.f, 0.2f, 1.f);
            for (LLVOCacheEntry* entry : region->getActiveCacheEntries())
            {
                if (drawn >= max_boxes)
                {
                    break;
                }
                if (!entry || entry->getParentID() > 0 || !entry->getEntry())
                {
                    continue;
                }
                LLDrawable* drawable = (LLDrawable*)entry->getEntry()->getDrawable();
                if (!drawable || drawable->isDead())
                {
                    continue;
                }
                const LLVector4a* ext = drawable->getSpatialExtents();
                LLVector4a center, half;
                center.setAdd(ext[0], ext[1]);
                center.mul(0.5f);
                half.setSub(ext[1], ext[0]);
                half.mul(0.5f);
                half.setAbs(half);
                LLVector4a o;
                o.load3(origin.mV);
                if (LLVOCacheGrid::distSquaredToBox(center, half, o) > r2)
                {
                    continue;
                }
                wireBox(center, half, LLVector3::zero);
                ++n_active;
                ++drawn;
            }
        }
        gGL.end();
    }
    gGL.flush();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);   // later overlay items in the segment draw untinted

    sLegend.clear();
    sLegend.push_back(llformat("FROZEN culling camera (Develop > Rendering) -- boxes within %.0f m%s", radius,
        drawn >= max_boxes ? " (capped)" : ""));
    sLegend.push_back("  white frustum, cyan draw-distance sphere, blue rear sphere");
    sLegend.push_back(llformat("  green instantiated %u   yellow waiting %u   orange selected/below threshold %u",
        n_active, n_waiting, n_below));
    sLegend.push_back(llformat("  grey not selected %u   magenta VRAM-held %u", n_unselected, n_held));
}

const std::vector<std::string>& legend()
{
    return sLegend;
}

} // namespace LLVOCacheDebug
