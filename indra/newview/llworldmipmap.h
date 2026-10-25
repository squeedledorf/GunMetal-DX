/**
 * @file llworldmipmap.h
 * @brief Data storage for the S3 mipmap of the entire world.
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLWORLDMIPMAP_H
#define LL_LLWORLDMIPMAP_H

#include <map>
#include <vector>
#include <utility>

#include "llmemory.h"           // LLPointer
#include "indra_constants.h"    // REGION_WIDTH_UNITS
#include "llregionhandle.h"     // to_region_handle()

class LLViewerFetchedTexture;

// LLWorldMipmap : Mipmap handling of all the tiles used to render the world at any resolution.
// This class provides a clean structured access to the hierarchy of tiles stored in the
// Amazon S3 repository and abstracts its directory/file structure.
// The interface of this class though still assumes that the caller knows the general level/tiles
// structure (at least, that it exists...) but doesn't requite the caller to know the details of it.
// IOW, you need to know that rendering levels exists as well as grid coordinates for regions,
// but you can ignore where those tiles are located, how to get them, etc...
// The class API gives you back LLPointer<LLViewerFetchedTexture> per tile.

// See llworldmipmapview.cpp for the implementation of a class who knows how to render an LLWorldMipmap.

// Implementation notes:
// - On the S3 servers, the tiles are rendered in 2 flavors: Objects and Terrain.
// - For the moment, LLWorldMipmap implements access only to the Objects tiles.
class LLWorldMipmap
{
public:
    // Parameters of the mipmap
    static const S32 MAP_LEVELS = 8;        // Number of subresolution levels computed by the mapserver
    static const S32 MAP_TILE_SIZE = 256;   // Width in pixels of the tiles computed by the mapserver

    LLWorldMipmap();
    ~LLWorldMipmap();

    // Clear up the maps and release all image handles
    void    reset();
    // Timer driven upkeep (NOT per draw): boost levels of wanted tiles, eviction of long idle ones
    void    equalizeBoostLevels();
    // Drop the boost levels to none (used when hiding the map)
    void    dropBoostLevels();

    // Draw side (read only): publish the range of tiles the view wants (grid coordinates of the first
    // and last tile, inclusive, stepping 1 << (level - 1) regions); cheap, no-op when unchanged
    void    setWantedTiles(S32 level, U32 grid_x0, U32 grid_y0, U32 grid_x1, U32 grid_y1);
    // Draw side (read only): look a tile up; never creates a texture nor changes boost levels.
    // NULL + pending = true: not created yet. NULL + pending = false: asset missing.
    LLViewerFetchedTexture* peekObjectsTile(U32 grid_x, U32 grid_y, S32 level, bool& pending) const;
    // Loader side: create the wanted tiles that do not exist yet, within a time budget (seconds)
    void    updateLoader(F64 budget_seconds);

    // Helper functions: those are here as they depend solely on the topology of the mipmap though they don't access it
    // Convert sim scale (given in sim width in display pixels) into a mipmap level
    static S32  scaleToLevel(F32 scale);
    // Convert world coordinates to mipmap grid coordinates at a given level
    static void globalToMipmap(F64 global_x, F64 global_y, S32 level, U32* grid_x, U32* grid_y);

    // Load the relevant tile from S3
    static LLPointer<LLViewerFetchedTexture> loadObjectsTile(U32 grid_x, U32 grid_y, S32 level);

private:
    // Get a handle (key) from grid coordinates
    U64     convertGridToHandle(U32 grid_x, U32 grid_y) { return to_region_handle(grid_x * REGION_WIDTH_UNITS, grid_y * REGION_WIDTH_UNITS); }

    // Clear a level from its "missing" tiles
    void cleanMissedTilesFromLevel(S32 level);

    // The mipmap is organized by resolution level (MAP_LEVELS of them). Each resolution level is an std::map
    // using a region_handle as a key and storing a smart pointer to the image as a value.
    typedef std::map<U64, LLPointer<LLViewerFetchedTexture> > sublevel_tiles_t;
    sublevel_tiles_t mWorldObjectsMipMap[MAP_LEVELS];
    // Frame (equalizeBoostLevels() call count) a tile was last requested with load on; parallel to the
    // maps above, used to evict long-idle tiles so the per-frame sweep stays bounded (#723)
    std::map<U64, F64> mTileLastUsed[MAP_LEVELS];
    // Wanted tile range, published by the view (#723)
    S32 mWantedLevel = 0;
    U32 mWantedX0 = 0, mWantedY0 = 0, mWantedX1 = 0, mWantedY1 = 0;
    F64 mWantedTime = 0.0;
    std::vector<std::pair<U32, U32> > mWantedOrder;     // wanted tiles, closest to view center first
    size_t mLoadCursor = 0;
    bool mBoostsDropped = false;
//  sublevel_tiles_t mWorldTerrainMipMap[MAP_LEVELS];

    S32 mCurrentLevel;      // The level last accessed by a getObjectsTile()
};

#endif // LL_LLWORLDMIPMAP_H
