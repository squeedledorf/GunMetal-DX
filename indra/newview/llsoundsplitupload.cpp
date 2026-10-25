/**
 * @file llsoundsplitupload.cpp
 * @brief Upload a long sound as several consecutive clips (#807).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llsoundsplitupload.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llclipboard.h"
#include "lldbstrings.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewermenufile.h"

#include <algorithm>
#include <cmath>

//-----------------------------------------------------------------------------
// One part's upload. Reports back to the batch when the upload finishes or fails.
//-----------------------------------------------------------------------------
namespace
{
class LLSoundPartUploadInfo : public LLNewFileResourceUploadInfo
{
public:
    LLSoundPartUploadInfo(std::shared_ptr<LLSoundSplitUpload> batch,
                          size_t index, size_t count,
                          const LLSoundSplitUpload::Params& p, const std::string& name,
                          bool show_inventory)
        : LLNewFileResourceUploadInfo(p.mSourceFile, name, p.mDescription, 0,
                                      LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                                      p.mNextOwnerPerms, p.mGroupPerms, p.mEveryonePerms,
                                      p.mCostPerClip, p.mDestFolder, show_inventory)
        , mBatch(batch)
        , mIndex(index)
        , mCount(count)
        , mPartName(name)
    {
    }

    // Shown in the modal "Uploading..." dialog: per-part progress.
    std::string getDisplayName() const override
    {
        LLStringUtil::format_map_t args;
        args["[INDEX]"] = llformat("%d", (S32)mIndex + 1);
        args["[COUNT]"] = llformat("%d", (S32)mCount);
        args["[NAME]"] = mPartName;
        return LLTrans::getString("SoundSplitProgress", args);
    }

    LLUUID finishUpload(LLSD& result) override
    {
        LLUUID item = LLNewFileResourceUploadInfo::finishUpload(result);
        const LLUUID asset = result["new_asset"].asUUID();
        report(asset.notNull(), asset, asset.notNull() ? std::string() : std::string("The server did not return a new asset."));
        return item;
    }

    bool failedUpload(LLSD& result, std::string& reason) override
    {
        report(false, LLUUID::null, reason);
        return false;   // let the normal error handling (notification) run too
    }

private:
    void report(bool ok, const LLUUID& asset, const std::string& reason)
    {
        if (mReported) return;
        mReported = true;
        if (mBatch)
        {
            mBatch->onPartFinished(mIndex, ok, asset, reason);
        }
    }

    std::shared_ptr<LLSoundSplitUpload> mBatch;
    size_t      mIndex;
    size_t      mCount;
    std::string mPartName;
    bool        mReported = false;
};

std::string join_names(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (i) out += ", ";
        out += names[i];
    }
    return out.empty() ? std::string("-") : out;
}
} // namespace

//-----------------------------------------------------------------------------

// static
double LLSoundSplitUpload::getSegmentSeconds()
{
    // Default 29.9 s leaves a safety margin below the 30 s limit so encoder padding cannot
    // push a part over it. Clamp whatever the setting says to stay usable.
    double secs = (double)gSavedSettings.getF32("SoundUploadSplitSeconds");
    if (!(secs >= 1.0)) secs = 1.0;
    const double max_secs = LLAUDIO_UPLOAD_LIMIT_SECONDS - 0.01;
    if (secs > max_secs) secs = max_secs;
    return secs;
}

// static
std::vector<LLAudioSplitPart> LLSoundSplitUpload::planFor(const LLAudioClip& clip)
{
    std::vector<LLAudioSplitPart> parts;
    if (clip.getDuration() <= LLAUDIO_UPLOAD_LIMIT_SECONDS)
    {
        LLAudioSplitPart whole;
        whole.mStart = 0;
        whole.mCount = clip.getFrames();
        parts.push_back(whole);
        return parts;
    }
    return llaudio_plan_split(clip.getFrames(), LLAUDIO_UPLOAD_SAMPLE_RATE, getSegmentSeconds());
}

// static
std::shared_ptr<LLSoundSplitUpload> LLSoundSplitUpload::create(const Params& params)
{
    return std::shared_ptr<LLSoundSplitUpload>(new LLSoundSplitUpload(params));
}

LLSoundSplitUpload::LLSoundSplitUpload(const Params& params)
    : mParams(params)
{
    const std::vector<LLAudioSplitPart> plan = planFor(*mParams.mClip);
    mParts.resize(plan.size());
    for (size_t i = 0; i < plan.size(); ++i)
    {
        mParts[i].mRange = plan[i];
        mParts[i].mName = llaudio_split_part_name(mParams.mBaseName, i + 1, plan.size(), DB_INV_ITEM_NAME_STR_LEN);
    }

    // Optional short fade at each cut (off by default so plain cuts join seamlessly).
    if (gSavedSettings.getBOOL("SoundUploadSplitFade"))
    {
        const double ms = std::max(0.0, std::min(50.0, (double)gSavedSettings.getF32("SoundUploadSplitFadeMs")));
        mFadeFrames = (size_t)std::llround(ms * (double)LLAUDIO_UPLOAD_SAMPLE_RATE / 1000.0);
    }
}

void LLSoundSplitUpload::start()
{
    if (mParts.empty()) return;
    LL_INFOS() << "Sound split upload: " << mParts.size() << " parts of '" << mParams.mBaseName << "'" << LL_ENDL;
    uploadPart(0);
}

void LLSoundSplitUpload::uploadPart(size_t index)
{
    if (index >= mParts.size()) return;
    Part& part = mParts[index];

    if (gDisconnected)
    {
        part.mState = State::Failed;
        part.mFailReason = LLTrans::getString("SoundSplitDisconnected");
        reportFailure(index);
        return;
    }
    if (gAgent.getRegionCapability("NewFileAgentInventory").empty())
    {
        // Without the capability the legacy asset path would not report back per part.
        part.mState = State::Failed;
        part.mFailReason = LLTrans::getString("SoundSplitNoCap");
        reportFailure(index);
        return;
    }

    part.mState = State::Uploading;
    part.mFailReason.clear();

    const bool last = (index + 1 == mParts.size());
    auto info = std::make_shared<LLSoundPartUploadInfo>(shared_from_this(), index, mParts.size(),
                                                        mParams, part.mName, last /* show inventory once, at the end */);

    // Fade only at interior cuts: no fade-in on the first part, no fade-out on the last.
    size_t fade = std::min(mFadeFrames, (size_t)(part.mRange.mCount / 2));
    info->setAudioClip(mParams.mClip, (size_t)part.mRange.mStart, (size_t)part.mRange.mCount,
                       index > 0 ? fade : 0, last ? 0 : fade);

    LL_INFOS() << "Sound split upload: part " << (index + 1) << "/" << mParts.size() << " '" << part.mName
        << "' frames " << part.mRange.mStart << ".." << (part.mRange.mStart + part.mRange.mCount) << LL_ENDL;

    LLResourceUploadInfo::ptr_t uploadInfo = info;
    upload_new_resource(uploadInfo);
}

void LLSoundSplitUpload::onPartFinished(size_t index, bool ok, const LLUUID& assetId, const std::string& reason)
{
    if (index >= mParts.size() || mParts[index].mState != State::Uploading) return;

    Part& part = mParts[index];
    if (ok)
    {
        part.mState = State::Done;
        part.mAssetId = assetId;
        scheduleNext(index);
    }
    else
    {
        part.mState = State::Failed;
        part.mFailReason = reason.empty() ? LLTrans::getString("SoundSplitUnknownReason") : reason;
        // Stop here: nothing further is uploaded until the user asks for a retry.
        auto self = shared_from_this();
        doOnIdleOneTime([self, index]() { self->reportFailure(index); });
    }
}

void LLSoundSplitUpload::scheduleNext(size_t index)
{
    auto self = shared_from_this();
    if (index + 1 < mParts.size())
    {
        // Next frame, once this upload coroutine has fully wound down.
        doOnIdleOneTime([self, index]() { self->uploadPart(index + 1); });
    }
    else
    {
        doOnIdleOneTime([self]() { self->reportDone(); });
    }
}

void LLSoundSplitUpload::reportFailure(size_t index)
{
    std::vector<std::string> done, remaining;
    for (const Part& p : mParts)
    {
        (p.mState == State::Done ? done : remaining).push_back(p.mName);
    }

    LLSD args;
    args["FILE"] = mParams.mBaseName;
    args["FAILED"] = mParts[index].mName;
    args["INDEX"] = llformat("%d", (S32)index + 1);
    args["COUNT"] = llformat("%d", (S32)mParts.size());
    args["REASON"] = mParts[index].mFailReason;
    args["DONE"] = join_names(done);
    args["REMAINING"] = join_names(remaining);
    args["REMAINING_COUNT"] = llformat("%d", (S32)remaining.size());
    args["REMAINING_COST"] = llformat("%d", (S32)remaining.size() * mParams.mCostPerClip);

    auto self = shared_from_this();
    LLNotificationsUtil::add("SoundSplitFailed", args, LLSD(),
        [self](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                self->retryRemaining();
            }
        });
}

void LLSoundSplitUpload::retryRemaining()
{
    // Explicit user request: resume at the first part that is not done.
    for (size_t i = 0; i < mParts.size(); ++i)
    {
        if (mParts[i].mState != State::Done)
        {
            mParts[i].mState = State::Pending;
            uploadPart(i);
            return;
        }
    }
}

std::string LLSoundSplitUpload::buildUuidList() const
{
    // A ready-made LSL list, in clip order, for scripts that play the clips back to back.
    std::string out = "list sounds = [\n";
    for (size_t i = 0; i < mParts.size(); ++i)
    {
        out += "    \"" + mParts[i].mAssetId.asString() + "\"";
        out += (i + 1 < mParts.size()) ? ",\n" : "\n";
    }
    out += "];\n";
    return out;
}

void LLSoundSplitUpload::reportDone()
{
    LLSD args;
    args["FILE"] = mParams.mBaseName;
    args["COUNT"] = llformat("%d", (S32)mParts.size());
    args["TOTAL"] = llformat("%d", getTotalCost());

    auto self = shared_from_this();
    LLNotificationsUtil::add("SoundSplitDone", args, LLSD(),
        [self](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
            {
                const LLWString text = utf8str_to_wstring(self->buildUuidList());
                LLClipboard::instance().copyToClipboard(text, 0, (S32)text.length());
            }
        });
}
