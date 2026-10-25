/**
 * @file slphotostudiodraw.h
 * @brief Anti-aliased 2D drawing for Photo Studio's graphs, wheels and scopes:
 *        a mitred polyline, the area under a polyline, and a washer whose
 *        colour varies around its sweep.
 *
 * Slipstream Photo Studio: these were upstream Alchemy additions to
 * llrender2dutils (ported with the Lightbox, credited); they moved here so
 * llrender is back to Linden's own helpers (#443).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2026, Alchemy Viewer Project.
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

#ifndef SL_PHOTOSTUDIODRAW_H
#define SL_PHOTOSTUDIODRAW_H

#include "v2math.h"
#include "v4color.h"

#include <vector>

/// Anti-aliased polyline: a mitred ribbon with a one-pixel alpha falloff.
void sl_polyline_2d(const std::vector<LLVector2>& points, const LLColor4& color,
                    F32 width = 1.f, bool closed = false);
/// The flat-filled area under a polyline, down to @a baseline_y.
void sl_polyfill_2d(const std::vector<LLVector2>& points, F32 baseline_y,
                    const LLColor4& color);
/// A washer whose colour varies around the sweep (one colour per step), e.g.
/// a hue ring; @a inner_fade scales alpha at the inner edge. Draws around the
/// current origin.
void sl_washer_angular_2d(F32 outer_radius, F32 inner_radius,
                          const std::vector<LLColor4>& colors, F32 inner_fade = 1.f);

#endif // SL_PHOTOSTUDIODRAW_H
