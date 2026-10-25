/**
 * @file   llgroupcolormap.cpp
 * @brief  Per-group nameplate tint registry.
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 */

#include "llviewerprecompiledheaders.h"

#include "llgroupcolormap.h"

#include "llagent.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcolorswatch.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llpanelgroup.h"    // LLPanelGroupTab
#include "llsdserialize.h"
#include "llsdutil_math.h"   // ll_sd_from_color4 / ll_color4_from_sd
#include "llstartup.h"
#include "llavatarpropertiesprocessor.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"      // LLVOAvatar::invalidateNameTags()
#include "message.h"

static constexpr char GROUP_COLOR_FILE[] = "settings_group_colors.xml";

static constexpr F32 IDLE_INTERVAL = 0.5f;          // seconds between avatar scans
static constexpr F64 GROUP_FETCH_RETRY = 30.0;      // re-ask if a groups reply never came
static constexpr F64 GROUP_PROBE_COOLDOWN = 30.0;   // Group Viewer re-probe throttle

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

LLGroupColorMap::LLGroupColorMap()
{
    gIdleCallbacks.addFunction(&LLGroupColorMap::onIdle, this);
}

LLGroupColorMap::~LLGroupColorMap()
{
    gIdleCallbacks.deleteFunction(&LLGroupColorMap::onIdle, this);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void LLGroupColorMap::setGroupColor(const LLUUID& group_id, const LLColor4& color)
{
    if (group_id.isNull())
        return;

    // Treat fully-transparent as "no color" (remove entry)
    if (color.mV[VW] < 0.01f)
    {
        clearGroupColor(group_id);
        return;
    }

    mColors[group_id] = color;
    saveToDisk();
    invalidateAllNameTags();
}

LLColor4 LLGroupColorMap::getGroupColor(const LLUUID& group_id) const
{
    if (group_id.isNull())
        return LLColor4::transparent;

    auto it = mColors.find(group_id);
    if (it != mColors.end())
        return it->second;

    return LLColor4::transparent;
}

bool LLGroupColorMap::hasGroupColor(const LLUUID& group_id) const
{
    if (group_id.isNull())
        return false;
    return mColors.find(group_id) != mColors.end();
}

void LLGroupColorMap::clearGroupColor(const LLUUID& group_id)
{
    if (mColors.erase(group_id) > 0)
    {
        saveToDisk();
        invalidateAllNameTags();
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// static
std::string LLGroupColorMap::getFilePath()
{
    std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "");
    if (!path.empty())
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, GROUP_COLOR_FILE);
    // Fall back to app_settings (before first login, path not yet set)
    return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, GROUP_COLOR_FILE);
}

void LLGroupColorMap::loadFromDisk()
{
    mColors.clear();
    mAvatars.clear();
    mPendingProbes.clear();

    const std::string filepath = getFilePath();
    if (!gDirUtilp->fileExists(filepath))
        return;

    llifstream stream(filepath.c_str());
    if (!stream.is_open())
    {
        LL_WARNS("GroupColor") << "Cannot open " << filepath << LL_ENDL;
        return;
    }

    LLSD data;
    if (LLSDSerialize::fromXMLDocument(data, stream) < 0)
    {
        LL_WARNS("GroupColor") << "Failed to parse " << filepath << LL_ENDL;
        return;
    }

    // Format: map of { group_uuid_string : [r, g, b, a] }
    for (LLSD::map_const_iterator it = data.beginMap(); it != data.endMap(); ++it)
    {
        LLUUID group_id(it->first);
        if (group_id.isNull()) continue;
        LLColor4 color = ll_color4_from_sd(it->second);
        if (color.mV[VW] >= 0.01f)
            mColors[group_id] = color;
    }

    LL_INFOS("GroupColor") << "Loaded " << mColors.size()
                           << " group color(s) from " << filepath << LL_ENDL;
}

void LLGroupColorMap::saveToDisk() const
{
    const std::string filepath = getFilePath();

    LLSD data = LLSD::emptyMap();
    for (const auto& [group_id, color] : mColors)
        data[group_id.asString()] = ll_sd_from_color4(color);

    llofstream stream(filepath.c_str());
    if (!stream.is_open())
    {
        LL_WARNS("GroupColor") << "Cannot write " << filepath << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(data, stream);
}

// ---------------------------------------------------------------------------
// Invalidation
// ---------------------------------------------------------------------------

// static
void LLGroupColorMap::invalidateAllNameTags()
{
    LLVOAvatar::invalidateNameTags();
}

// ---------------------------------------------------------------------------
// Active group of an avatar
// ---------------------------------------------------------------------------

LLUUID LLGroupColorMap::getActiveGroupID(const LLUUID& avatar_id) const
{
    if (avatar_id == gAgentID)
        return gAgent.getGroupID();

    auto it = mAvatars.find(avatar_id);
    return it != mAvatars.end() ? it->second.mGroupID : LLUUID::null;
}

LLColor4 LLGroupColorMap::tint(const LLUUID& avatar_id, const LLColor4& color) const
{
    if (mColors.empty())
        return color;

    const LLColor4 group_color = getGroupColor(getActiveGroupID(avatar_id));
    return group_color.mV[VW] >= 0.01f ? group_color : color;
}

// Only generate traffic when something will use the answer: a tint is
// configured, or the Group Viewer floater is open.
bool LLGroupColorMap::resolutionWanted() const
{
    return hasAnyColors() || LLFloaterReg::instanceVisible("fs_group_viewer");
}

// static
void LLGroupColorMap::onIdle(void* data)
{
    static_cast<LLGroupColorMap*>(data)->idle();
}

// In SoapStorm the fetch fired from LLVOAvatar::idleUpdateNameTagText when an
// avatar's Title NameValue changed. Scanning here instead also resolves
// avatars whose nametags are hidden, so their minimap dots get tinted too.
void LLGroupColorMap::idle()
{
    if (mIdleTimer.getElapsedTimeF32() < IDLE_INTERVAL)
        return;
    mIdleTimer.reset();

    if (LLStartUp::getStartupState() < STATE_STARTED || !resolutionWanted())
        return;

    const F64 now = LLFrameTimer::getTotalSeconds();
    ++mPass;

    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatarp = static_cast<LLVOAvatar*>(character);
        if (!avatarp || avatarp->isDead() || avatarp->isSelf() || avatarp->isControlAvatar())
            continue;

        LLNameValue* title_nv = avatarp->getNVPair("Title");
        const std::string title = (title_nv && title_nv->getString()) ? title_nv->getString() : "";

        AvatarGroupState& state = mAvatars[avatarp->getID()];
        state.mSeenPass = mPass;

        // Title changed (a new state starts with an empty title): the active
        // group may have changed, resolve it again.
        if (title != state.mTitle)
        {
            const bool had_group = state.mGroupID.notNull();
            state.mTitle = title;
            state.mGroupID.setNull();
            state.mResolved = false;
            state.mProbeWanted = false;
            state.mLastFetchTime = 0.0;
            if (had_group)
                avatarp->clearNameTag();
        }

        // No visible group tag implies no active group (nothing to infer,
        // and no false tints from stale gear on group-less avatars).
        if (state.mTitle.empty())
            continue;

        if (!state.mResolved && now - state.mLastFetchTime > GROUP_FETCH_RETRY)
        {
            // The sim answers AvatarPropertiesRequest with an AvatarGroupsReply
            // carrying the avatar's title in each of its (visible) groups.
            state.mLastFetchTime = now;
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest(avatarp->getID());
        }

        // Probe deferred because no attachments had streamed in yet.
        probeAttachmentGroups(avatarp, state);
    }

    // Forget avatars that are gone.
    for (auto it = mAvatars.begin(); it != mAvatars.end(); )
    {
        it = (it->second.mSeenPass != mPass) ? mAvatars.erase(it) : std::next(it);
    }
}

// static
void LLGroupColorMap::processAvatarGroupsReply(LLMessageSystem* msg)
{
    if (!instanceExists())
        return;
    LLGroupColorMap& self = instance();

    LLUUID avatar_id;
    msg->getUUIDFast(_PREHASH_AgentData, _PREHASH_AvatarID, avatar_id);

    auto it = self.mAvatars.find(avatar_id);
    if (it == self.mAvatars.end() || it->second.mResolved || it->second.mTitle.empty())
        return;
    AvatarGroupState& state = it->second;
    state.mResolved = true;

    // The active group is the one whose GroupTitle matches the avatar's
    // current Title NameValue.
    state.mGroupID.setNull();
    const S32 group_count = msg->getNumberOfBlocksFast(_PREHASH_GroupData);
    for (S32 i = 0; i < group_count; ++i)
    {
        std::string group_title;
        msg->getStringFast(_PREHASH_GroupData, _PREHASH_GroupTitle, group_title, i);
        if (group_title == state.mTitle)
        {
            msg->getUUIDFast(_PREHASH_GroupData, _PREHASH_GroupID, state.mGroupID, i);
            break;
        }
    }

    LLViewerObject* obj = gObjectList.findObject(avatar_id);
    LLVOAvatar* avatarp = obj ? obj->asAvatar() : nullptr;

    if (state.mGroupID.isNull() && avatarp)
    {
        // No match, but a group tag is showing: the profile group list omits
        // groups the avatar has set to hidden, so read the active group from a
        // worn attachment instead.
        state.mProbeWanted = true;
        self.probeAttachmentGroups(avatarp, state);
    }

    if (avatarp)
        avatarp->clearNameTag();
}

// ---------------------------------------------------------------------------
// Attachment-group fallback
// ---------------------------------------------------------------------------

// The profile group list omits hidden groups, so the title match can fail
// even though a group tag is clearly showing. The sim keeps worn attachments'
// group in sync with the wearer's active group, and their properties are
// publicly inspectable (same data as Inspect), so the group of any worn
// attachment IS the active group. Mesh/texture load state is irrelevant: the
// sim answers from its own object data as soon as the attachment's
// ObjectUpdate has arrived.
void LLGroupColorMap::probeAttachmentGroups(LLVOAvatar* avatarp, AvatarGroupState& state)
{
    if (!state.mProbeWanted)
        return;

    if (state.mTitle.empty() || !resolutionWanted())
    {
        state.mProbeWanted = false;
        return;
    }

    // Stale-entry backstop: replies the sim never sent would accumulate.
    if (mPendingProbes.size() > 256)
        mPendingProbes.clear();

    // First reply is authoritative; ask two attachments only for redundancy
    // against a dropped reply.
    constexpr S32 MAX_GROUP_PROBES = 2;
    S32 sent = 0;
    LLMessageSystem* msg = gMessageSystem;
    for (const auto& [point_id, attachment] : avatarp->mAttachmentPoints)
    {
        // Note: no getValid() check here. mValid is a render/LOD flag set
        // lazily on first LOD update; attachments are often fully bound but
        // not yet "valid" during a login/teleport object flood, and we only
        // need UUID + local id + region to query properties.
        if (!attachment)
            continue;

        for (const LLPointer<LLViewerObject>& attached_object : attachment->mAttachedObjects)
        {
            if (sent >= MAX_GROUP_PROBES)
                break;
            LLViewerRegion* regionp = attached_object ? attached_object->getRegion() : nullptr;
            if (!regionp || attached_object->isDead() || attached_object->isHUDAttachment())
                continue;

            mPendingProbes[attached_object->getID()] = avatarp->getID();

            // RequestObjectPropertiesFamily is not answered for attached
            // objects; a brief server-side select+deselect is (same approach
            // FSAreaSearch uses). The selection never enters the viewer-side
            // selection list, so the user's selection is unaffected.
            msg->newMessageFast(_PREHASH_ObjectSelect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, attached_object->getLocalID());
            msg->sendReliable(regionp->getHost());

            msg->newMessageFast(_PREHASH_ObjectDeselect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, attached_object->getLocalID());
            msg->sendReliable(regionp->getHost());

            ++sent;
        }
        if (sent >= MAX_GROUP_PROBES)
            break;
    }

    if (sent > 0)
    {
        state.mProbeWanted = false;
    }
    // else: the avatar's attachment ObjectUpdates haven't streamed in yet
    // (still rezzing); the idle scan retries.
}

// Re-arm the probe for an avatar whose group never resolved (hidden group
// with the probe disarmed at the time, lost replies, etc). Cooldown keeps
// the Group Viewer's periodic refresh from generating repeated traffic for
// avatars that genuinely cannot be resolved.
void LLGroupColorMap::requestGroupProbeIfUnresolved(const LLUUID& avatar_id)
{
    auto it = mAvatars.find(avatar_id);
    if (it == mAvatars.end())
        return;
    AvatarGroupState& state = it->second;
    if (state.mGroupID.notNull() || state.mTitle.empty() || !state.mResolved)
        return;

    const F64 now = LLFrameTimer::getTotalSeconds();
    if (now - state.mLastProbeTime < GROUP_PROBE_COOLDOWN)
        return;
    state.mLastProbeTime = now;

    LLViewerObject* obj = gObjectList.findObject(avatar_id);
    LLVOAvatar* avatarp = obj ? obj->asAvatar() : nullptr;
    if (!avatarp || avatarp->isDead())
        return;

    state.mProbeWanted = true;
    probeAttachmentGroups(avatarp, state);
}

// static
bool LLGroupColorMap::handleAttachmentGroupReply(const LLUUID& object_id, const LLUUID& group_id)
{
    if (!instanceExists())
        return false;
    LLGroupColorMap& self = instance();

    auto probe = self.mPendingProbes.find(object_id);
    if (probe == self.mPendingProbes.end())
        return false;

    const LLUUID avatar_id = probe->second;
    self.mPendingProbes.erase(probe);

    auto it = self.mAvatars.find(avatar_id);
    if (it == self.mAvatars.end())
        return true;

    if (group_id.notNull() && it->second.mGroupID != group_id)
    {
        it->second.mGroupID = group_id;
        LLVOAvatar::invalidateNameTag(avatar_id);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Group profile color swatch
// ---------------------------------------------------------------------------

// static
void LLGroupColorMap::initGroupColorSwatch(LLPanelGroupTab* panel)
{
    LLColorSwatchCtrl* swatch = panel->findChild<LLColorSwatchCtrl>("group_nametag_color");
    if (!swatch)
        return;

    auto apply = [panel, swatch](LLUICtrl*, const LLSD&)
    {
        if (panel->getGroupID().notNull())
            getInstance()->setGroupColor(panel->getGroupID(), swatch->get());
    };
    swatch->setCanApplyImmediately(false);
    swatch->setCommitCallback(apply);
    swatch->setOnSelectCallback(apply);
    swatch->setOnCancelCallback([panel](LLUICtrl*, const LLSD&) { refreshGroupColorSwatch(panel); });

    if (LLButton* clear_btn = panel->findChild<LLButton>("group_nametag_color_clear"))
    {
        clear_btn->setCommitCallback([panel](LLUICtrl*, const LLSD&)
        {
            if (panel->getGroupID().notNull())
                getInstance()->clearGroupColor(panel->getGroupID());
            refreshGroupColorSwatch(panel);
        });
    }

    refreshGroupColorSwatch(panel);
}

// static
void LLGroupColorMap::refreshGroupColorSwatch(LLPanelGroupTab* panel)
{
    LLColorSwatchCtrl* swatch = panel->findChild<LLColorSwatchCtrl>("group_nametag_color");
    if (!swatch)
        return;

    const LLColor4 stored = getInstance()->getGroupColor(panel->getGroupID());
    swatch->set(stored.mV[VW] >= 0.01f ? stored : LLColor4::white, false);
}
