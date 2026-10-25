/**
 * @file llterrainsmooth.h
 * @brief The smooth terrain surface through the region height samples (#211).
 *
 * A monotone bicubic: one Hermite patch per grid cell, from the cell's four corner
 * heights and their x / y tangents. Each tangent is the mean of the two neighbouring
 * secants, limited the Fritsch-Carlson way (at most 3x the smaller secant, zero where
 * the two secants disagree in sign), and the value is held to the cell's corner range.
 * The surface is C1, passes exactly through every sample, and never overshoots: a
 * cliff becomes an S-curve inside its own cell instead of a lip above it (Catmull-Rom
 * grows metres of ridge on a 30 m cliff). Ported from upstream Alchemy 820dbaa0ab
 * (ALTerrainSurfaceMaps::smoothHeight); same arithmetic, same order.
 *
 * Along a cell edge the value is a 1-D monotone Hermite of that row's two samples and
 * their limited slopes, which stays within the two end values, so the corner clamp is
 * a no-op there and two cells (patches, regions) sharing an edge agree on it exactly.
 *
 * Consumers: LLSurface::resolveHeightRegion / resolveNormalGlobal (picks, foot
 * planting, parcel lines, camera collision) and the VK terrain mesh generator
 * (llcleanterrainsmooth.cpp). The simulator still collides with the linear surface;
 * the two differ by less than one cell's rise anywhere.
 */
#pragma once

#include "stdtypes.h"
#include "llmath.h"

#include <cmath>

namespace LLTerrainSmooth
{
// The tangent at the middle of three samples, limited so the cubic between it and
// either neighbour stays monotone.
inline F32 limitedSlope(F32 left, F32 mid, F32 right)
{
    const F32 dl = mid - left;
    const F32 dr = right - mid;
    if (dl * dr <= 0.f)
    {
        return 0.f;
    }
    const F32 m = 0.5f * (dl + dr);
    const F32 bound = 3.f * llmin(fabsf(dl), fabsf(dr));
    return llclamp(m, -bound, bound);
}

// Cubic Hermite basis at t (value weights of the two ends, tangent weights) and its
// derivative.
inline void hermite(F32 t, F32 (&h)[4], F32 (&d)[4])
{
    const F32 t2 = t * t;
    const F32 t3 = t2 * t;
    h[0] =  2.f * t3 - 3.f * t2 + 1.f;
    h[1] = -2.f * t3 + 3.f * t2;
    h[2] =        t3 - 2.f * t2 + t;
    h[3] =        t3 -       t2;
    d[0] =  6.f * t2 - 6.f * t;
    d[1] = -6.f * t2 + 6.f * t;
    d[2] =  3.f * t2 - 4.f * t + 1.f;
    d[3] =  3.f * t2 - 2.f * t;
}

// The smooth height at (x, y) in GRID units and its gradient per grid unit.
// sample(gx, gy) supplies the heights, including one sample past the region edge (it
// owns the reach into neighbouring regions / the clamp where there is none).
template <class Sample>
F32 smoothHeight(F32 x, F32 y, const Sample& sample, F32* dzdx, F32* dzdy)
{
    const S32 ix = (S32)floorf(x);
    const S32 iy = (S32)floorf(y);
    const F32 u = x - (F32)ix;
    const F32 v = y - (F32)iy;

    F32 r0[4], r1[4];
    for (S32 k = 0; k < 4; ++k)
    {
        r0[k] = sample(ix + k - 1, iy);
        r1[k] = sample(ix + k - 1, iy + 1);
    }
    const F32 c0m = sample(ix,     iy - 1);
    const F32 c0p = sample(ix,     iy + 2);
    const F32 c1m = sample(ix + 1, iy - 1);
    const F32 c1p = sample(ix + 1, iy + 2);

    const F32 f00 = r0[1], f10 = r0[2], f01 = r1[1], f11 = r1[2];
    const F32 fx00 = limitedSlope(r0[0], f00, f10);
    const F32 fx10 = limitedSlope(f00, f10, r0[3]);
    const F32 fx01 = limitedSlope(r1[0], f01, f11);
    const F32 fx11 = limitedSlope(f01, f11, r1[3]);
    const F32 fy00 = limitedSlope(c0m, f00, f01);
    const F32 fy01 = limitedSlope(f00, f01, c0p);
    const F32 fy10 = limitedSlope(c1m, f10, f11);
    const F32 fy11 = limitedSlope(f10, f11, c1p);

    F32 hu[4], du[4], hv[4], dv[4];
    hermite(u, hu, du);
    hermite(v, hv, dv);

    const F32 h   = hu[0] * (hv[0] * f00 + hv[1] * f01) + hu[1] * (hv[0] * f10 + hv[1] * f11)
                  + hu[2] * (hv[0] * fx00 + hv[1] * fx01) + hu[3] * (hv[0] * fx10 + hv[1] * fx11)
                  + hu[0] * (hv[2] * fy00 + hv[3] * fy01) + hu[1] * (hv[2] * fy10 + hv[3] * fy11);
    if (dzdx)
    {
        *dzdx = du[0] * (hv[0] * f00 + hv[1] * f01) + du[1] * (hv[0] * f10 + hv[1] * f11)
              + du[2] * (hv[0] * fx00 + hv[1] * fx01) + du[3] * (hv[0] * fx10 + hv[1] * fx11)
              + du[0] * (hv[2] * fy00 + hv[3] * fy01) + du[1] * (hv[2] * fy10 + hv[3] * fy11);
    }
    if (dzdy)
    {
        *dzdy = hu[0] * (dv[0] * f00 + dv[1] * f01) + hu[1] * (dv[0] * f10 + dv[1] * f11)
              + hu[2] * (dv[0] * fx00 + dv[1] * fx01) + hu[3] * (dv[0] * fx10 + dv[1] * fx11)
              + hu[0] * (dv[2] * fy00 + dv[3] * fy01) + hu[1] * (dv[2] * fy10 + dv[3] * fy11);
    }
    const F32 lo = llmin(llmin(f00, f10), llmin(f01, f11));
    const F32 hi = llmax(llmax(f00, f10), llmax(f01, f11));
    return llclamp(h, lo, hi);
}
} // namespace LLTerrainSmooth
