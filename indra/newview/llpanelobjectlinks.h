/**
 * @file llpanelobjectlinks.h
 * @brief Build floater "Links" tab: the selected linkset's links and the current
 *        link's faces, with hover preview (VK selection outline) and click-to-select.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
 * $/LicenseInfo$
 */

#ifndef LL_LLPANELOBJECTLINKS_H
#define LL_LLPANELOBJECTLINKS_H

#include "llpanel.h"
#include "lluuid.h"

#include <boost/signals2.hpp>

class LLFilterEditor;
class LLScrollListCtrl;
class LLScrollListItem;
class LLTextBox;
class LLViewerObject;

// #252: Links / Faces list in the build tools.
//   Links: every link of the selected linkset (root first), link number + name (desc
//   in the tooltip); filter by name / desc / number. Non-editable links are listed
//   greyed and can't be selected.
//   Faces: the faces of the current link: index, base texture thumbnail (PBR base
//   colour or legacy diffuse, fetched as a <=256 px BOOST_THUMBNAIL copy; 256 px
//   tooltip preview), colour swatch (TE colour, or PBR base colour factor; alpha over
//   a checkerboard), material type (PBR / legacy), alpha mode, texture id.
//   Hovering a row previews it in the VK selection outline (getHoverTarget, polled by
//   the frame builder); clicking selects it with the build tools' semantics
//   (LLToolSelect "Edit linked parts" / LLToolFace), Ctrl / Shift toggling.
class LLPanelObjectLinks final : public LLPanel
{
public:
    // #969: true when the current selection is a face pick (not a whole link).
    bool isFacePick() const;
    LLPanelObjectLinks();
    ~LLPanelObjectLinks() override;

    bool postBuild() override;
    void refresh() override;
    void onVisibilityChange(bool new_visibility) override;
    bool handleToolTip(S32 x, S32 y, MASK mask) override;

    // Hover preview for the selection outline: the link (te = -1: every face) or face
    // under the mouse in a visible list. False when nothing is hovered.
    static bool getHoverTarget(LLUUID& object_id, S32& te);

    // ObjectPropertiesFamily reply: true if it answers a name request of ours.
    static bool processPropertiesFamily(const LLUUID& id, const std::string& name, const std::string& desc);

private:
    struct LinkRow
    {
        LLUUID id;
        S32    link_num = 0;
        bool   editable = false;
    };

    void onFilterEdit();
    void onLinkCommit();
    void onFaceCommit();

    LLViewerObject* currentRoot() const;
    void rebuildLinks(LLViewerObject* root);
    void rebuildFaces();
    void syncListSelection();
    void requestMissingNames();
    void updateNameCell(const LLUUID& id);
    LLScrollListItem* clickedItem(LLScrollListCtrl* list) const;
    static bool linkEditable(LLViewerObject* obj);     // modify permission (row shown grey without)
    static bool linkSelectable(LLViewerObject* obj);   // may be selected (no modify needed)
    static LLScrollListItem* hoveredItem(LLScrollListCtrl* list, S32 mx, S32 my);
    // Idle: keep the face thumbnails' fetch alive and re-record the retained floater
    // when one of them changes what it draws (loaded / sharper / missing).
    static void onIdleThumbs(void* data);
    void pollThumbs();

    LLScrollListCtrl* mLinksList = nullptr;
    LLScrollListCtrl* mFacesList = nullptr;
    LLFilterEditor*   mFilterEditor = nullptr;
    LLTextBox*        mLinksCount = nullptr;
    LLTextBox*        mFacesLabel = nullptr;

    LLUUID               mRootID;
    std::vector<LinkRow> mLinks;          // the whole linkset, root first
    std::string          mLinksKey;       // ids + editability + filter of the shown rows
    std::string          mFilter;         // lower-case
    LLUUID               mFaceLinkID;     // link whose faces are listed
    LLUUID               mLastClickedLink;// last link clicked in the list (face source)
    std::string          mFacesKey;
    bool                 mSyncing = false;
    bool                 mFacePickCommit = false;  // #969: last list commit was a face (vs a link)
    U64                  mThumbStamp = 0;  // last pollThumbs() state of the face thumbnails

    boost::signals2::scoped_connection mSelectionConnection;

    static LLPanelObjectLinks* sInstance;
};

#endif // LL_LLPANELOBJECTLINKS_H
