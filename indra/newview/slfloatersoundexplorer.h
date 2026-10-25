/**
 * @file slfloatersoundexplorer.h
 * @brief Sound Explorer floater: browse recently played sounds, preview and block them (#654)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsoundlog.h)
 * $/LicenseInfo$
 */

#ifndef SL_FLOATERSOUNDEXPLORER_H
#define SL_FLOATERSOUNDEXPLORER_H

#include "llfloater.h"
#include "lleventtimer.h"
#include "llavatarnamecache.h"
#include "slsoundlog.h"

#include <map>

class LLButton;
class LLScrollListCtrl;
class LLTextBox;

class SLFloaterSoundExplorer : public LLFloater
{
    friend class LLFloaterReg;
public:
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    SLFloaterSoundExplorer(const LLSD& key);
    ~SLFloaterSoundExplorer() override;

    struct Selected
    {
        LLUUID mSound;
        LLUUID mSource;
    };

    void refresh();
    void updateButtons();
    void setObjectFilter(const LLUUID& object_id);
    std::vector<Selected> getSelected() const;
    const SLSoundLogEntry* findEntry(const LLUUID& sound, const LLUUID& source) const;
    std::string resolveOwnerName(const LLUUID& owner_id);
    std::string resolveSourceName(const SLSoundLogEntry& entry);
    LLVector3d currentPosition(const SLSoundLogEntry& entry) const;

    void onPlay();
    void onLookAt();
    void onBeacon();
    void onCopyUUID();
    void onFilterBySource();
    void onClearLog();
    void onBlockSound();
    void onUnblockSound();
    void onBlockSource();
    void onBlockOwner();

    LLScrollListCtrl* mList = nullptr;
    LLTextBox* mCounter = nullptr;
    LLTextBox* mObjectFilterLabel = nullptr;
    LLButton* mClearObjectFilterBtn = nullptr;

    LLUUID mObjectFilter;
    bool mDirty = true;
    LLFrameTimer mRefreshTimer;
    boost::signals2::connection mLogConn;
    boost::signals2::connection mBlocklistConn;
    std::map<LLUUID, LLAvatarNameCache::callback_connection_t> mNameRequests;
};

#endif // SL_FLOATERSOUNDEXPLORER_H
