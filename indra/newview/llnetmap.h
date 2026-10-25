/**
 * @file llnetmap.h
 * @brief A little map of the world with network information
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#ifndef LL_LLNETMAP_H
#define LL_LLNETMAP_H

#include "llmath.h"
#include "lluictrl.h"
#include "v3math.h"
#include "v3dmath.h"
#include "v4color.h"
#include "llpointer.h"
#include "llcoord.h"

class LLColor4U;
class LLImageRaw;
class LLViewerTexture;
class LLFloaterMap;
class LLMenuGL;
class LLViewerRegion;

class LLNetMap final : public LLUICtrl
{
public:
    struct Params
    :   public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<LLUIColor> bg_color;

        Params()
        :   bg_color("bg_color")
        {}
    };

protected:
    LLNetMap (const Params & p);
    friend class LLUICtrlFactory;
    friend class LLFloaterMap;

public:
    virtual ~LLNetMap();

    static const F32 MAP_SCALE_MIN;
    static const F32 MAP_SCALE_FAR;
    static const F32 MAP_SCALE_MEDIUM;
    static const F32 MAP_SCALE_CLOSE;
    static const F32 MAP_SCALE_VERY_CLOSE;
    static const F32 MAP_SCALE_MAX;

    /*virtual*/ void    draw();
    /*virtual*/ bool    handleScrollWheel(S32 x, S32 y, S32 clicks);
    /*virtual*/ bool    handleMouseDown(S32 x, S32 y, MASK mask);
    /*virtual*/ bool    handleMouseUp(S32 x, S32 y, MASK mask);
    /*virtual*/ bool    handleHover( S32 x, S32 y, MASK mask );
    void                onMouseLeave(S32 x, S32 y, MASK mask) override;   // <GMDX> cursor coordinates
    /*virtual*/ bool    handleToolTip( S32 x, S32 y, MASK mask);
    /*virtual*/ void    reshape(S32 width, S32 height, bool called_from_parent = true);

    /*virtual*/ bool    postBuild();
    /*virtual*/ bool    handleRightMouseDown( S32 x, S32 y, MASK mask );
    /*virtual*/ bool    handleClick(S32 x, S32 y, MASK mask);
    bool                fitPanAt(S32 x, S32 y) const; // <GMDX>
    /*virtual*/ bool    handleDoubleClick( S32 x, S32 y, MASK mask );

    void            refreshParcelOverlay() { mUpdateParcelImage = true; }

    void            setScale(F32 scale);

    void            setToolTipMsg(const std::string& msg) { mToolTipMsg = msg; }
    void            setParcelNameMsg(const std::string& msg) { mParcelNameMsg = msg; }
    void            setParcelSalePriceMsg(const std::string& msg) { mParcelSalePriceMsg = msg; }
    void            setParcelSaleAreaMsg(const std::string& msg) { mParcelSaleAreaMsg = msg; }
    void            setParcelOwnerMsg(const std::string& msg) { mParcelOwnerMsg = msg; }
    void            setRegionNameMsg(const std::string& msg) { mRegionNameMsg = msg; }
    void            setToolTipHintMsg(const std::string& msg) { mToolTipHintMsg = msg; }
    void            setAltToolTipHintMsg(const std::string& msg) { mAltToolTipHintMsg = msg; }

    void            renderScaledPointGlobal( const LLVector3d& pos, const LLColor4U &color, F32 radius );

    // <GMDX> "Show Whole Region" (GMDXMiniMapWholeRegion) as last drawn: the agent's region
    // fitted to the view, north up, no pan / zoom.
    bool            isWholeRegion() const { return mFitRegion; }
    // </GMDX>

private:
    const LLVector3d& getObjectImageCenterGlobal()  { return mObjectImageCenterGlobal; }
    void            renderPoint(const LLVector3 &pos, const LLColor4U &color,
                                S32 diameter, S32 relative_height = 0);

    LLVector3       globalPosToView(const LLVector3d& global_pos);
    LLVector3d      viewPosToGlobal(S32 x,S32 y);

    void            drawTracking( const LLVector3d& pos_global,
                                  const LLColor4& color,
                                  bool draw_arrow = true);
    bool            isMouseOnPopupMenu();
    void            updateAboutLandPopupButton();
    bool            handleToolTipAgent(const LLUUID& avatar_id);
    static void     showAvatarInspector(const LLUUID& avatar_id);

    bool            createImage(LLPointer<LLImageRaw>& rawimagep) const;
    void            createObjectImage();
    void            createParcelImage();
    void            renderPropertyLinesForRegion(LLViewerRegion* pRegion, const LLColor4U& clrOverlay);

    F32             getScaleForName(std::string scale_name);

    // <GMDX> Show Whole Region
    void            updateScaleDerived();   // pixels per meter, dot size, object layer texels per meter
    LLVector3d      mapCenterGlobal() const;
    void            clampFitView(F32 region_width);   // <GMDX> Whole Region zoom
    LLVector2       mapPan() const { return mFitRegion ? mFitPan : mCurPan; }
    bool            mCanRecenter = false;   // <GMDX> the menu's "Re-center now" (enable callback)
    LLVector2       mFitPan;   // Show Whole Region: the fitted square's offset from the view centre (heading tape room)
    bool            rotateMap() const;
    // </GMDX>
    // <GMDX> Only Render Physical Objects
    void            drawPhysicalObjects(const LLVector3& map_center_agent, F32 pixels_per_meter, const LLViewerRegion* only_region);
    // </GMDX>
    // <GMDX> Mini-map marks (gmdxminimapmarks.h) and the coordinates under the cursor
    void            drawMarks();
    void            drawCursorCoordinates();
    bool            onMap(const LLVector3& pos_local) const;   // inside the drawn map (Whole Region: its square)
    LLUUID          markAt(S32 x, S32 y);                       // the mark drawn under (x, y), or null
    void            copyPopupPosition(bool slurl);
    void            addMarkAtPopup(S32 color);
    // Zones: Draw Zone puts the map in a drawing mode (mDrawKind: GMDXMapZone::Kind, -1 = off)
    void            drawZones();          // the saved zones: under the avatars
    void            drawZonePreview();    // the zone being drawn and its hint: over everything
    std::vector<LLVector2> zoneOutline(S32 kind, const std::vector<LLVector3d>& points);   // in view coordinates
    LLUUID          zoneAt(S32 x, S32 y);
    void            startZone(S32 kind);
    void            finishZone();
    S32             mDrawKind = -1;
    std::vector<LLVector3d> mDrawPoints;
    LLVector3d      mDrawCursor;
    bool            mDrawDrag = false;
    LLUUID          mPopupZone;                                 // the zone the menu was opened on
    bool            mCursorInside = false;
    S32             mCursorX = 0, mCursorY = 0;
    LLUUID          mPopupMark;                                 // the mark the menu was opened on
    // </GMDX>
    static bool     outsideSlop(S32 x, S32 y, S32 start_x, S32 start_y, S32 slop);

private:
    bool            mUpdateObjectImage;
    bool            mUpdateParcelImage;

    LLUIColor       mBackgroundColor;

    F32             mScale;                 // Size of a region in pixels
    F32             mPixelsPerMeter;        // world meters to map pixels
    F32             mObjectMapTPM;          // texels per meter on map
    F32             mObjectMapPixels;       // Width of object map in pixels
    F32             mDotRadius;             // Size of avatar markers

    // <GMDX> Show Whole Region, as last drawn: the map centre (the region's centre, at the
    // camera's height) and scale replace the camera position and mScale.
    bool            mFitRegion = false;
    F32             mFitPixelsPerMeter = 0.f;
    LLVector3d      mFitCenterGlobal;
    // Whole Region zoom: 1 = the whole region; the view's centre in region metres (clamped so the
    // view stays inside the region); the region they belong to; the zoom-1 scale and the map's
    // square in view coordinates (for the wheel / drag handlers).
    F32             mFitZoom = 1.f;
    LLVector2       mFitViewCenter;
    U64             mFitRegionHandle = 0;
    F32             mFitBasePixelsPerMeter = 0.f;
    LLRectf         mFitMapRect;
    // </GMDX>

    bool            mPanning; // map is being dragged
    bool            mCentering; // map is being re-centered around the agent
    LLVector2       mCurPan;
    LLVector2       mStartPan; // pan offset at start of drag
    LLVector3d      mPopupWorldPos; // world position picked under mouse when context menu is opened
    LLCoordGL       mMouseDown; // pointer position at start of drag

    LLVector3d      mObjectImageCenterGlobal;      // centre of the pass being BUILT (plotting)
    LLVector3d      mObjectImageShownCenterGlobal; // centre of the image currently uploaded (drawing)
    // Incremental object layer: a pass plots a slice of the map objects per frame into
    // mObjectRawImagep and uploads it when complete (the texture keeps the last complete
    // layer meanwhile). It was a full re-plot of every object in the region, 10x a second,
    // on the main thread -- ~3 ms per frame average in a dense sim.
    bool            mObjectPassActive = false;
    size_t          mObjectPassCursor = 0;
    LLPointer<LLImageRaw> mObjectRawImagep;
    LLPointer<LLViewerTexture>  mObjectImagep;

    LLVector3d      mParcelImageCenterGlobal;
    LLPointer<LLImageRaw> mParcelRawImagep;
    LLPointer<LLViewerTexture>  mParcelImagep;

    boost::signals2::connection mParcelMgrConn;
    boost::signals2::connection mParcelOverlayConn;

    LLUUID          mClosestAgentToCursor;
    uuid_vec_t      mAvatarsAtLastRightClick; // avatars under the cursor, closest first

    std::string     mToolTipMsg;
    std::string     mParcelNameMsg;
    std::string     mParcelSalePriceMsg;
    std::string     mParcelSaleAreaMsg;
    std::string     mParcelOwnerMsg;
    std::string     mRegionNameMsg;
    std::string     mToolTipHintMsg;
    std::string     mAltToolTipHintMsg;

public:
    void            setSelected(uuid_vec_t uuids) { gmSelected=uuids; };

private:
    bool isZoomChecked(const LLSD& userdata);
    void setZoom(const LLSD& userdata);
    void handleStopTracking(const LLSD& userdata);
    void activateCenterMap(const LLSD& userdata);
    bool isMapOrientationChecked(const LLSD& userdata);
    void setMapOrientation(const LLSD& userdata);
    void popupShowAboutLand(const LLSD& userdata);
    void handleCamToAvatar(const LLSD& userdata);
    void updateCamToAvatarMenu(LLMenuGL* menu, S32 x, S32 y);

    LLHandle<LLView> mPopupMenuHandle;
    uuid_vec_t      gmSelected;
};


#endif
