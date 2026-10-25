/**
 * @file slfloatersoundexplorer.cpp
 * @brief Sound Explorer floater: browse recently played sounds, preview and block them (#654)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsoundlog.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slfloatersoundexplorer.h"

#include <set>

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llavatarname.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llmutelist.h"
#include "llscrolllistctrl.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltracker.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "slassetblocklist.h"

namespace
{
    constexpr F32 PERIODIC_REFRESH_SECONDS = 1.0f;
    const char SEP = '|';

    std::string makeRowKey(const LLUUID& sound, const LLUUID& source)
    {
        return sound.asString() + SEP + source.asString();
    }

    bool parseRowKey(const std::string& key, LLUUID& sound, LLUUID& source)
    {
        const size_t pos = key.find(SEP);
        if (pos == std::string::npos)
        {
            return false;
        }
        sound.set(key.substr(0, pos));
        source.set(key.substr(pos + 1));
        return true;
    }
}

SLFloaterSoundExplorer::SLFloaterSoundExplorer(const LLSD& key)
    : LLFloater(key)
{
}

SLFloaterSoundExplorer::~SLFloaterSoundExplorer()
{
    mLogConn.disconnect();
    mBlocklistConn.disconnect();
    for (auto& [id, conn] : mNameRequests)
    {
        conn.disconnect();
    }
}

bool SLFloaterSoundExplorer::postBuild()
{
    mList = getChild<LLScrollListCtrl>("sound_list");
    mCounter = getChild<LLTextBox>("counter");
    mObjectFilterLabel = getChild<LLTextBox>("object_filter_label");
    mClearObjectFilterBtn = getChild<LLButton>("clear_object_filter");

    mList->setCommitOnSelectionChange(true);
    mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
    mList->setDoubleClickCallback([this]() { onLookAt(); });

    getChild<LLButton>("play_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPlay(); });
    getChild<LLButton>("look_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onLookAt(); });
    getChild<LLButton>("beacon_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBeacon(); });
    getChild<LLButton>("copy_uuid_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onCopyUUID(); });
    getChild<LLButton>("filter_object_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onFilterBySource(); });
    getChild<LLButton>("clear_log_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onClearLog(); });
    getChild<LLButton>("block_sound_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBlockSound(); });
    getChild<LLButton>("unblock_sound_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUnblockSound(); });
    getChild<LLButton>("block_source_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBlockSource(); });
    getChild<LLButton>("block_owner_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onBlockOwner(); });
    getChild<LLButton>("blocklist_btn")->setClickedCallback([](LLUICtrl*, const LLSD&)
    {
        LLFloaterReg::showInstance("blocked", LLSD("asset_tab"));
    });
    mClearObjectFilterBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { setObjectFilter(LLUUID::null); });

    // Any filter change re-lists immediately.
    for (const char* name : { "show_attached", "show_triggered", "show_collision", "playing_only" })
    {
        getChild<LLUICtrl>(name)->setCommitCallback([this](LLUICtrl*, const LLSD&) { mDirty = true; });
    }

    mLogConn = SLSoundLog::getInstance()->setChangeCallback([this]() { mDirty = true; });
    mBlocklistConn = SLAssetBlocklist::getInstance()->setChangeCallback([this]() { mDirty = true; });

    updateButtons();
    return true;
}

void SLFloaterSoundExplorer::onOpen(const LLSD& key)
{
    // Opened from the object menu: key carries the object to filter on. Plain menu opens
    // (no key) show everything.
    setObjectFilter(key.isMap() && key.has("object_id") ? key["object_id"].asUUID() : LLUUID::null);
    mDirty = true;
}

void SLFloaterSoundExplorer::draw()
{
    SLSoundLog::getInstance()->flushPending();

    if (mDirty || mRefreshTimer.getElapsedTimeF32() > PERIODIC_REFRESH_SECONDS)
    {
        refresh();
    }
    LLFloater::draw();
}

void SLFloaterSoundExplorer::setObjectFilter(const LLUUID& object_id)
{
    mObjectFilter = object_id;
    const bool filtered = object_id.notNull();
    mObjectFilterLabel->setVisible(filtered);
    mClearObjectFilterBtn->setVisible(filtered);
    if (filtered)
    {
        std::string name;
        if (LLViewerObject* objectp = gObjectList.findObject(object_id))
        {
            if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp))
            {
                name = node->mName;
            }
        }
        if (name.empty())
        {
            name = object_id.asString();
        }
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        mObjectFilterLabel->setText(getString("ObjectFilter", args));
    }
    mDirty = true;
}

std::vector<SLFloaterSoundExplorer::Selected> SLFloaterSoundExplorer::getSelected() const
{
    std::vector<Selected> out;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        Selected s;
        if (parseRowKey(item->getValue().asString(), s.mSound, s.mSource))
        {
            out.push_back(s);
        }
    }
    return out;
}

const SLSoundLogEntry* SLFloaterSoundExplorer::findEntry(const LLUUID& sound, const LLUUID& source) const
{
    for (const SLSoundLogEntry& e : SLSoundLog::getInstance()->getEntries())
    {
        if (e.mSoundID == sound && e.mSourceID == source)
        {
            return &e;
        }
    }
    return nullptr;
}

std::string SLFloaterSoundExplorer::resolveOwnerName(const LLUUID& owner_id)
{
    if (owner_id.isNull())
    {
        return getString("Unknown");
    }
    LLAvatarName av_name;
    if (LLAvatarNameCache::get(owner_id, &av_name))
    {
        return av_name.getUserName();
    }
    if (mNameRequests.find(owner_id) == mNameRequests.end())
    {
        mNameRequests[owner_id] = LLAvatarNameCache::get(owner_id,
            [this](const LLUUID&, const LLAvatarName&) { mDirty = true; });
    }
    return LLTrans::getString("LoadingData");
}

std::string SLFloaterSoundExplorer::resolveSourceName(const SLSoundLogEntry& entry)
{
    if (entry.mSourceID.isNull())
    {
        return getString("SourceUnknown");
    }
    if (LLViewerObject* objectp = gObjectList.findObject(entry.mSourceID))
    {
        if (objectp->isAvatar())
        {
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(entry.mSourceID, &av_name))
            {
                return av_name.getDisplayName();
            }
        }
        else if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp))
        {
            if (!node->mName.empty())
            {
                return node->mName;
            }
        }
    }
    // Object names are only known for selected objects; fall back to a short id.
    return entry.mSourceID.asString().substr(0, 8);
}

LLVector3d SLFloaterSoundExplorer::currentPosition(const SLSoundLogEntry& entry) const
{
    if (entry.mSourceID.notNull())
    {
        if (LLViewerObject* objectp = gObjectList.findObject(entry.mSourceID))
        {
            return objectp->getPositionGlobal();
        }
    }
    return entry.mPosGlobal;
}

void SLFloaterSoundExplorer::refresh()
{
    mDirty = false;
    mRefreshTimer.reset();

    const bool show_attached = gSavedSettings.getBOOL("SoundExplorerShowAttached");
    const bool show_triggered = gSavedSettings.getBOOL("SoundExplorerShowTriggered");
    const bool show_collision = gSavedSettings.getBOOL("SoundExplorerShowCollision");
    const bool playing_only = gSavedSettings.getBOOL("SoundExplorerPlayingOnly");

    // Keep the selection and scroll position across the rebuild.
    std::vector<std::string> selected_keys;
    for (LLScrollListItem* item : mList->getAllSelected())
    {
        selected_keys.push_back(item->getValue().asString());
    }
    const S32 scroll_pos = mList->getScrollPos();

    mList->clearRows();

    const LLVector3d agent_pos = gAgent.getPositionGlobal();
    const F64 now = LLDate::now().secondsSinceEpoch();
    const SLSoundLog& log = *SLSoundLog::getInstance();
    SLAssetBlocklist& blocklist = *SLAssetBlocklist::getInstance();
    S32 shown = 0;

    for (const SLSoundLogEntry& e : log.getEntries())
    {
        switch (e.mType)
        {
        case SLSoundLogEntry::TYPE_ATTACHED:  if (!show_attached)  continue; break;
        case SLSoundLogEntry::TYPE_TRIGGERED: if (!show_triggered) continue; break;
        case SLSoundLogEntry::TYPE_COLLISION: if (!show_collision) continue; break;
        }
        if (mObjectFilter.notNull() && e.mSourceID != mObjectFilter)
        {
            continue;
        }
        const bool playing = SLSoundLog::isPlaying(e);
        if (playing_only && !playing)
        {
            continue;
        }

        const bool blocked = blocklist.isBlocked(e.mSoundID);

        const char* type_str = "TypeTriggered";
        if (e.mType == SLSoundLogEntry::TYPE_ATTACHED) type_str = "TypeAttached";
        else if (e.mType == SLSoundLogEntry::TYPE_COLLISION) type_str = "TypeCollision";

        std::string status = getString("StatusIdle");
        if (blocked) status = getString("StatusBlocked");
        else if (playing) status = getString("StatusPlaying");

        std::string type = getString(type_str);
        if (e.mLoop)
        {
            type += " (loop)";
        }

        const F64 dist = (currentPosition(e) - agent_pos).magVec();
        const S64 age = (S64)std::max(0.0, now - e.mLastPlayed);
        std::string last;
        {
            LLStringUtil::format_map_t args;
            if (age < 60)        { args["[N]"] = llformat("%lld", (long long)age);          last = getString("AgoSeconds", args); }
            else if (age < 3600) { args["[N]"] = llformat("%lld", (long long)(age / 60));   last = getString("AgoMinutes", args); }
            else                 { args["[N]"] = llformat("%lld", (long long)(age / 3600)); last = getString("AgoHours", args); }
        }

        LLStringUtil::format_map_t name_args;
        name_args["[ID]"] = e.mSoundID.asString().substr(0, 8);

        LLSD row;
        row["value"] = makeRowKey(e.mSoundID, e.mSourceID);
        LLSD& cols = row["columns"];
        cols[0]["column"] = "name";     cols[0]["value"] = getString("SoundName", name_args);
        cols[1]["column"] = "owner";    cols[1]["value"] = resolveOwnerName(e.mOwnerID);
        cols[2]["column"] = "source";   cols[2]["value"] = resolveSourceName(e);
        cols[3]["column"] = "distance"; cols[3]["value"] = llformat("%.0fm", dist);
        cols[4]["column"] = "type";     cols[4]["value"] = type;
        cols[5]["column"] = "status";   cols[5]["value"] = status;
        cols[6]["column"] = "last";     cols[6]["value"] = last;
        mList->addElement(row, ADD_BOTTOM);
        ++shown;
    }

    for (const std::string& key : selected_keys)
    {
        mList->setSelectedByValue(LLSD(key), true);
    }
    mList->setScrollPos(scroll_pos);

    LLStringUtil::format_map_t counter_args;
    counter_args["[SHOWN]"] = llformat("%d", shown);
    counter_args["[TOTAL]"] = llformat("%d", (S32)log.size());
    mCounter->setText(getString("Counter", counter_args));

    updateButtons();
}

void SLFloaterSoundExplorer::updateButtons()
{
    const std::vector<Selected> sel = getSelected();
    const bool any = !sel.empty();
    bool any_blocked = false;
    bool any_unblocked = false;
    bool any_source = false;
    bool any_owner = false;
    for (const Selected& s : sel)
    {
        if (SLAssetBlocklist::getInstance()->isBlocked(s.mSound)) any_blocked = true; else any_unblocked = true;
        if (s.mSource.notNull()) any_source = true;
        if (const SLSoundLogEntry* e = findEntry(s.mSound, s.mSource))
        {
            if (e->mOwnerID.notNull() && e->mOwnerID != gAgentID) any_owner = true;
        }
    }
    const bool single = sel.size() == 1;
    getChildView("play_btn")->setEnabled(single && any_unblocked);
    getChildView("look_btn")->setEnabled(single);
    getChildView("beacon_btn")->setEnabled(single);
    getChildView("copy_uuid_btn")->setEnabled(any);
    getChildView("filter_object_btn")->setEnabled(single && any_source);
    getChildView("block_sound_btn")->setEnabled(any_unblocked);
    getChildView("unblock_sound_btn")->setEnabled(any_blocked);
    getChildView("block_source_btn")->setEnabled(any_source);
    getChildView("block_owner_btn")->setEnabled(any_owner);
}

void SLFloaterSoundExplorer::onPlay()
{
    const std::vector<Selected> sel = getSelected();
    if (sel.size() != 1 || !gAudiop || SLAssetBlocklist::getInstance()->isBlocked(sel[0].mSound))
    {
        return;
    }
    // Same path as the inventory sound preview: a non-positional, forced-priority local source.
    gAudiop->preloadSound(sel[0].mSound);
    gAudiop->triggerSound(sel[0].mSound, gAgentID, 1.0f, LLAudioEngine::AUDIO_TYPE_SFX);
}

void SLFloaterSoundExplorer::onLookAt()
{
    const std::vector<Selected> sel = getSelected();
    if (sel.size() != 1)
    {
        return;
    }
    if (const SLSoundLogEntry* e = findEntry(sel[0].mSound, sel[0].mSource))
    {
        gAgentCamera.setFocusOnAvatar(false, ANIMATE);
        gAgentCamera.setFocusGlobal(currentPosition(*e), e->mSourceID);
    }
}

void SLFloaterSoundExplorer::onBeacon()
{
    const std::vector<Selected> sel = getSelected();
    if (sel.size() != 1)
    {
        return;
    }
    if (const SLSoundLogEntry* e = findEntry(sel[0].mSound, sel[0].mSource))
    {
        LLTracker::trackLocation(currentPosition(*e), resolveSourceName(*e), "", LLTracker::LOCATION_ITEM);
    }
}

void SLFloaterSoundExplorer::onCopyUUID()
{
    std::string text;
    for (const Selected& s : getSelected())
    {
        if (!text.empty())
        {
            text += "\n";
        }
        text += s.mSound.asString();
    }
    if (!text.empty())
    {
        LLClipboard::instance().copyToClipboard(utf8str_to_wstring(text), 0, (S32)text.length());
    }
}

void SLFloaterSoundExplorer::onFilterBySource()
{
    const std::vector<Selected> sel = getSelected();
    if (sel.size() == 1 && sel[0].mSource.notNull())
    {
        setObjectFilter(sel[0].mSource);
    }
}

void SLFloaterSoundExplorer::onClearLog()
{
    SLSoundLog::getInstance()->clear();
    mDirty = true;
}

void SLFloaterSoundExplorer::onBlockSound()
{
    std::set<LLUUID> done;
    for (const Selected& s : getSelected())
    {
        if (!done.insert(s.mSound).second)
        {
            continue;
        }
        const SLSoundLogEntry* e = findEntry(s.mSound, s.mSource);
        // addEntry also silences every instance that is playing right now.
        SLAssetBlocklist::getInstance()->addEntry(s.mSound, e ? e->mOwnerID : LLUUID::null,
                                                  e ? e->mRegion : std::string(), LLAssetType::AT_SOUND);
    }
    mDirty = true;
}

void SLFloaterSoundExplorer::onUnblockSound()
{
    uuid_vec_t ids;
    for (const Selected& s : getSelected())
    {
        ids.push_back(s.mSound);
    }
    SLAssetBlocklist::getInstance()->removeEntries(ids);
    mDirty = true;
}

void SLFloaterSoundExplorer::onBlockSource()
{
    for (const Selected& s : getSelected())
    {
        if (s.mSource.isNull() || s.mSource == gAgentID)
        {
            continue;
        }
        LLViewerObject* objectp = gObjectList.findObject(s.mSource);
        if (objectp && objectp->isAvatar())
        {
            // The source is an avatar (footsteps, typing, gesture sounds): silence its object sounds.
            LLAvatarName av_name;
            LLAvatarNameCache::get(s.mSource, &av_name);
            LLMuteList::getInstance()->add(LLMute(s.mSource, av_name.getUserName(), LLMute::AGENT), LLMute::flagObjectSounds);
            continue;
        }
        std::string name;
        if (objectp)
        {
            if (LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(objectp))
            {
                name = node->mName;
            }
        }
        if (name.empty())
        {
            name = s.mSource.asString();
        }
        LLMuteList::getInstance()->add(LLMute(s.mSource, name, LLMute::OBJECT));
    }
    mDirty = true;
}

void SLFloaterSoundExplorer::onBlockOwner()
{
    std::set<LLUUID> done;
    for (const Selected& s : getSelected())
    {
        const SLSoundLogEntry* e = findEntry(s.mSound, s.mSource);
        if (!e || e->mOwnerID.isNull() || e->mOwnerID == gAgentID || !done.insert(e->mOwnerID).second)
        {
            continue;
        }
        // Owner-level "object sounds" partial mute: same as Communicate > Blocked > object sounds.
        LLAvatarName av_name;
        LLAvatarNameCache::get(e->mOwnerID, &av_name);
        LLMuteList::getInstance()->add(LLMute(e->mOwnerID, av_name.getUserName(), LLMute::AGENT), LLMute::flagObjectSounds);
    }
    mDirty = true;
}
