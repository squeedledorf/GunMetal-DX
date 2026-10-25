/**
 * @file llworldmipmap.cpp
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

#include "llviewerprecompiledheaders.h"

#include "llworldmipmap.h"
#include "llviewercontrol.h"        // LLControlGroup

#include "llviewertexturelist.h"
#include "lltimer.h"
#include <algorithm>
#include "math.h"   // log()

// Loader tuning (#723). The draw never creates or boosts tiles: it only reads what is resident and
// publishes the wanted tile range; updateLoader() (timer driven, outside draw) creates them.
static constexpr S32 MAX_NEW_TILES_PER_TICK = 8;
static constexpr F64 TILE_IDLE_EVICT_SECONDS = 60.0;
static constexpr F64 WANTED_STALE_SECONDS = 1.0;

LLWorldMipmap::LLWorldMipmap() :
    mCurrentLevel(0)
{
}

LLWorldMipmap::~LLWorldMipmap()
{
    reset();
}

// Delete all sublevel maps and clean them
void LLWorldMipmap::reset()
{
    for (int level = 0; level < MAP_LEVELS; level++)
    {
        mWorldObjectsMipMap[level].clear();
        mTileLastUsed[level].clear();
    }
    mWantedOrder.clear();
    mWantedLevel = 0;
    mLoadCursor = 0;
}

// Draw side: publish the range of tiles (grid coordinates of the first and last tile, inclusive,
// stepping 1 << (level - 1) regions) the view currently wants. Cheap; does nothing if unchanged.
void LLWorldMipmap::setWantedTiles(S32 level, U32 grid_x0, U32 grid_y0, U32 grid_x1, U32 grid_y1)
{
    mWantedTime = LLTimer::getElapsedSeconds();
    if (level == mWantedLevel && grid_x0 == mWantedX0 && grid_y0 == mWantedY0 &&
        grid_x1 == mWantedX1 && grid_y1 == mWantedY1)
    {
        return;
    }
    mWantedLevel = level;
    mWantedX0 = grid_x0;
    mWantedY0 = grid_y0;
    mWantedX1 = grid_x1;
    mWantedY1 = grid_y1;
    mLoadCursor = 0;

    // Build the load order: closest to the center of the view first
    const U32 step = 1u << (level - 1);
    mWantedOrder.clear();
    for (U32 y = grid_y0; y <= grid_y1; y += step)
    {
        for (U32 x = grid_x0; x <= grid_x1; x += step)
        {
            mWantedOrder.push_back(std::make_pair(x, y));
        }
    }
    const F32 cx = 0.5f * ((F32)grid_x0 + (F32)grid_x1);
    const F32 cy = 0.5f * ((F32)grid_y0 + (F32)grid_y1);
    std::sort(mWantedOrder.begin(), mWantedOrder.end(),
              [cx, cy](const std::pair<U32, U32>& a, const std::pair<U32, U32>& b)
              {
                  F32 da = ((F32)a.first - cx) * ((F32)a.first - cx) + ((F32)a.second - cy) * ((F32)a.second - cy);
                  F32 db = ((F32)b.first - cx) * ((F32)b.first - cx) + ((F32)b.second - cy) * ((F32)b.second - cy);
                  return da < db;
              });
}

// Draw side: read-only lookup. Never creates a texture, never touches boost levels.
// Returns NULL with pending = true if the tile has not been created yet, NULL with pending = false if
// the asset is known to be missing, the tile otherwise.
LLViewerFetchedTexture* LLWorldMipmap::peekObjectsTile(U32 grid_x, U32 grid_y, S32 level, bool& pending) const
{
    pending = false;
    const sublevel_tiles_t& level_mipmap = mWorldObjectsMipMap[level-1];
    sublevel_tiles_t::const_iterator found = level_mipmap.find(to_region_handle(grid_x * REGION_WIDTH_UNITS, grid_y * REGION_WIDTH_UNITS));
    if (found == level_mipmap.end())
    {
        pending = true;
        return NULL;
    }
    LLViewerFetchedTexture* img = found->second.get();
    if (img->isMissingAsset())
    {
        return NULL;
    }
    return img;
}

// Loader side (timer driven, outside draw): create/request the wanted tiles that do not exist yet,
// at a bounded rate. Returns when done or when the time budget is spent.
void LLWorldMipmap::updateLoader(F64 budget_seconds)
{
    if (mWantedLevel == 0 || mLoadCursor >= mWantedOrder.size())
    {
        return;
    }
    if (LLTimer::getElapsedSeconds() - mWantedTime > WANTED_STALE_SECONDS)
    {
        return; // map not being drawn
    }
    // If the *loading* level changed, clear the new level from "missed" tiles
    // so that we get a chance to reload them
    if (mWantedLevel != mCurrentLevel)
    {
        cleanMissedTilesFromLevel(mWantedLevel);
        mCurrentLevel = mWantedLevel;
    }
    const F64 start = LLTimer::getElapsedSeconds();
    S32 created = 0;
    sublevel_tiles_t& level_mipmap = mWorldObjectsMipMap[mWantedLevel-1];
    while (mLoadCursor < mWantedOrder.size())
    {
        const std::pair<U32, U32>& t = mWantedOrder[mLoadCursor++];
        U64 handle = convertGridToHandle(t.first, t.second);
        if (level_mipmap.find(handle) != level_mipmap.end())
        {
            continue;
        }
        level_mipmap.insert(sublevel_tiles_t::value_type(handle, loadObjectsTile(t.first, t.second, mWantedLevel)));
        mTileLastUsed[mWantedLevel-1][handle] = start;
        if (++created >= MAX_NEW_TILES_PER_TICK || (LLTimer::getElapsedSeconds() - start) > budget_seconds)
        {
            break;
        }
    }
}

// Timer-driven upkeep (not per frame): tiles of the wanted range get BOOST_MAP, everything else drops to
// BOOST_NONE, and tiles nobody wanted for a long while are forgotten so the sweep stays bounded.
// Also drops everything when the map has not been drawn for a while.
void LLWorldMipmap::equalizeBoostLevels()
{
    const F64 now = LLTimer::getElapsedSeconds();
    const bool stale = (now - mWantedTime) > WANTED_STALE_SECONDS;
    if (stale && mBoostsDropped)
    {
        return;
    }
    mBoostsDropped = stale;
    const U32 step = (mWantedLevel > 0) ? (1u << (mWantedLevel - 1)) : 1u;

    for (S32 level = 0; level < MAP_LEVELS; level++)
    {
        sublevel_tiles_t& level_mipmap = mWorldObjectsMipMap[level];
        std::map<U64, F64>& last_used = mTileLastUsed[level];
        for (sublevel_tiles_t::iterator iter = level_mipmap.begin(); iter != level_mipmap.end(); )
        {
            LLViewerFetchedTexture* img = iter->second.get();
            bool wanted = false;
            if (!stale && (level + 1) == mWantedLevel)
            {
                LLVector3d origin = from_region_handle(iter->first);
                U32 gx = (U32)(origin.mdV[VX] / REGION_WIDTH_METERS);
                U32 gy = (U32)(origin.mdV[VY] / REGION_WIDTH_METERS);
                wanted = gx >= mWantedX0 && gx <= mWantedX1 && gy >= mWantedY0 && gy <= mWantedY1 &&
                         ((gx - mWantedX0) % step) == 0 && ((gy - mWantedY0) % step) == 0;
            }
            if (wanted)
            {
                last_used[iter->first] = now;
                img->setBoostLevel(LLGLTexture::BOOST_MAP);
            }
            else
            {
                img->setBoostLevel(LLGLTexture::BOOST_NONE);
                std::map<U64, F64>::iterator lu = last_used.find(iter->first);
                if (lu != last_used.end() && (now - lu->second) > TILE_IDLE_EVICT_SECONDS)
                {
                    last_used.erase(lu);
                    iter = level_mipmap.erase(iter);
                    continue;
                }
            }
            ++iter;
        }
    }
}

// This method should be used when the mipmap is not actively used for a while, e.g., the map UI is hidden
void LLWorldMipmap::dropBoostLevels()
{
    // For each level
    for (S32 level = 0; level < MAP_LEVELS; level++)
    {
        sublevel_tiles_t& level_mipmap = mWorldObjectsMipMap[level];
        // For each tile
        for (sublevel_tiles_t::iterator iter = level_mipmap.begin(); iter != level_mipmap.end(); iter++)
        {
            LLPointer<LLViewerFetchedTexture> img = iter->second;
            img->setBoostLevel(LLGLTexture::BOOST_NONE);
        }
    }
    mBoostsDropped = true;
}

//static
LLPointer<LLViewerFetchedTexture> LLWorldMipmap::loadObjectsTile(U32 grid_x, U32 grid_y, S32 level)
{
    // Get the grid coordinates
    std::string imageurl = gSavedSettings.getString("CurrentMapServerURL") + llformat("map-%d-%d-%d-objects.jpg", level, grid_x, grid_y);

    // DO NOT COMMIT!! DEBUG ONLY!!!
    // Use a local jpeg for every tile to test map speed without S3 access
    //imageurl = "file://C:\\Develop\\mapserver-distribute-3\\indra\\build-vc80\\mapserver\\relwithdebinfo\\regions\\00995\\01001\\region-995-1001-prims.jpg";
    // END DEBUG
    //LL_INFOS("WorldMap") << "LLWorldMipmap::loadObjectsTile(), URL = " << imageurl << LL_ENDL;

    LLPointer<LLViewerFetchedTexture> img = LLViewerTextureManager::getFetchedTextureFromUrl(imageurl, FTT_MAP_TILE, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    LL_DEBUGS("MAPURL") << "fetching map tile from " << imageurl << LL_ENDL;

    img->setBoostLevel(LLGLTexture::BOOST_MAP);

    // Return the smart pointer
    return img;
}

// This method is used to clean up a level from tiles marked as "missing".
// The idea is to allow tiles that have been improperly marked missing to be reloaded when retraversing the level again.
// When zooming in and out rapidly, some tiles are never properly loaded and, eventually marked missing.
// This creates "blue" areas in a subresolution that never got a chance to reload if we don't clean up the level.
void LLWorldMipmap::cleanMissedTilesFromLevel(S32 level)
{
    // Check the input data
    llassert(level <= MAP_LEVELS);
    llassert(level >= 0);

    // This happens when the object is first initialized
    if (level == 0)
    {
        return;
    }

    // Iterate through the subresolution level and suppress the tiles that are marked as missing
    // Note: erasing in a map while iterating through it is bug prone. Using a postfix increment is mandatory here.
    sublevel_tiles_t& level_mipmap = mWorldObjectsMipMap[level-1];
    sublevel_tiles_t::iterator it = level_mipmap.begin();
    while (it != level_mipmap.end())
    {
        LLPointer<LLViewerFetchedTexture> img = it->second;
        if (img->isMissingAsset())
        {
            level_mipmap.erase(it++);
        }
        else
        {
            ++it;
        }
    }
    return;
}

// static methods
// Compute the level in the world mipmap (between 1 and MAP_LEVELS, as in the URL) given the scale (size of a sim in screen pixels)
S32 LLWorldMipmap::scaleToLevel(F32 scale)
{
    // If scale really small, picks up the higest level there is (lowest resolution)
    if (scale <= F32_MIN)
        return MAP_LEVELS;
    // Compute the power of two resolution level knowing the base level
    S32 level = llfloor((log(REGION_WIDTH_METERS/scale)/log(2.0f)) + 1.0f);
    // Check bounds and return the value
    if (level > MAP_LEVELS)
        return MAP_LEVELS;
    else if (level < 1)
        return 1;
    else
        return level;
}

// Convert world coordinates to mipmap grid coordinates at a given level (between 1 and MAP_LEVELS)
void LLWorldMipmap::globalToMipmap(F64 global_x, F64 global_y, S32 level, U32* grid_x, U32* grid_y)
{
    // Check the input data
    llassert(level <= MAP_LEVELS);
    llassert(level >= 1);

    // Convert world coordinates into grid coordinates
    *grid_x = lltrunc(global_x/REGION_WIDTH_METERS);
    *grid_y = lltrunc(global_y/REGION_WIDTH_METERS);
    // Compute the valid grid coordinates at that level of the mipmap
    S32 regions_in_tile = 1 << (level - 1);
    *grid_x = *grid_x - (*grid_x % regions_in_tile);
    *grid_y = *grid_y - (*grid_y % regions_in_tile);
}


