/**
 * @file   llgroupcolormap.h
 * @brief  Per-group nameplate tint registry.
 *
 * Stores a client-side color for each group UUID.  When the group color is
 * set, every avatar whose *active* group tag matches that UUID gets their
 * nameplate (and minimap dot) rendered in that color instead of the default.
 *
 * Data is persisted to  <account_dir>/settings_group_colors.xml  so colors
 * survive relogs.  The file is keyed by group UUID string → LLColor4 LLSD.
 *
 * GunMetal-DX: the active-group resolution (Title NameValue matched against
 * the AvatarGroupsReply titles, with the worn-attachment group as fallback
 * for hidden groups) lived inside LLVOAvatar in SoapStorm. It is kept here,
 * keyed by avatar id, so LLVOAvatar only needs a one-line colour hook.
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 only.
 * $/LicenseInfo$
 */

#ifndef LL_LLGROUPCOLORMAP_H
#define LL_LLGROUPCOLORMAP_H

#include "llframetimer.h"
#include "llsingleton.h"
#include "lluuid.h"
#include "v4color.h"
#include <map>
#include <string>
#include <unordered_map>

class LLMessageSystem;
class LLPanelGroupTab;
class LLVOAvatar;

class LLGroupColorMap : public LLSingleton<LLGroupColorMap>
{
    LLSINGLETON(LLGroupColorMap);
    ~LLGroupColorMap();
    LOG_CLASS(LLGroupColorMap);

public:
    // ---- Color CRUD ---------------------------------------------------------

    /** Set (or clear) the nameplate tint for a group.
     *  Pass LLColor4::transparent (alpha == 0) to remove the color entry. */
    void setGroupColor(const LLUUID& group_id, const LLColor4& color);

    /** Return the tint for @p group_id, or LLColor4::transparent if none. */
    LLColor4 getGroupColor(const LLUUID& group_id) const;

    /** True if a non-transparent color is stored for this group. */
    bool hasGroupColor(const LLUUID& group_id) const;

    /** True if any group has a color configured; used to skip group lookups
     *  (and the attachment-group probe) entirely when tinting is unused. */
    bool hasAnyColors() const { return !mColors.empty(); }

    /** Remove the color entry for this group. */
    void clearGroupColor(const LLUUID& group_id);

    // ---- Persistence --------------------------------------------------------
    /** Loads this account's colors and forgets avatar group state from any
     *  previous session. Called once per login. */
    void loadFromDisk();
    void saveToDisk() const;

    // ---- Cache invalidation -------------------------------------------------
    /** Called after a color is changed so nametags rebuild on next idle. */
    static void invalidateAllNameTags();

    // ---- Active group of an avatar -----------------------------------------
    /** The avatar's active group, or null while unknown. Own avatar: the
     *  agent's active group. */
    LLUUID getActiveGroupID(const LLUUID& avatar_id) const;

    /** The avatar's group color if one is set, otherwise @p color. */
    LLColor4 tint(const LLUUID& avatar_id, const LLColor4& color) const;

    /** Re-arm the attachment-group probe for a still-unresolved avatar
     *  (cooldown-throttled). Used by the Group Viewer floater. */
    void requestGroupProbeIfUnresolved(const LLUUID& avatar_id);

    /** Hook from LLAvatarPropertiesProcessor::processAvatarGroupsReply. */
    static void processAvatarGroupsReply(LLMessageSystem* msg);

    /** Hook from LLSelectMgr::processObjectProperties: routes replies for
     *  probed attachments back to the wearer. True if the object was ours. */
    static bool handleAttachmentGroupReply(const LLUUID& object_id, const LLUUID& group_id);

    // ---- Group profile color swatch ----------------------------------------
    /** Wires the "group_nametag_color" swatch and its Clear button. */
    static void initGroupColorSwatch(LLPanelGroupTab* panel);
    static void refreshGroupColorSwatch(LLPanelGroupTab* panel);

private:
    static std::string getFilePath();

    struct AvatarGroupState
    {
        std::string mTitle;              // Title NameValue the state belongs to
        LLUUID      mGroupID;            // active group; null until known
        F64         mLastFetchTime{ 0.0 };
        F64         mLastProbeTime{ 0.0 };
        U32         mSeenPass{ 0 };
        bool        mResolved{ false };  // groups reply processed for mTitle
        bool        mProbeWanted{ false }; // title match failed; probe when attachments are available
    };

    static void onIdle(void* data);
    void idle();
    bool resolutionWanted() const;
    void probeAttachmentGroups(LLVOAvatar* avatar, AvatarGroupState& state);

    // group_uuid → RGBA color  (std::hash<LLUUID> is specialised in lluuid.h)
    std::unordered_map<LLUUID, LLColor4> mColors;

    std::unordered_map<LLUUID, AvatarGroupState> mAvatars;  // avatar id → state
    std::map<LLUUID, LLUUID> mPendingProbes;                // object id → avatar id
    LLFrameTimer mIdleTimer;
    U32 mPass{ 0 };
};

#endif // LL_LLGROUPCOLORMAP_H
