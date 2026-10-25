/**
 * @file llsoundsplitupload.h
 * @brief Upload a long sound as several consecutive clips (#807).
 *
 * Second Life rejects sound uploads longer than 30 seconds. This splits decoded audio at
 * exact sample boundaries into ~SoundUploadSplitSeconds parts and uploads them one at a
 * time through the normal inventory upload path. Nothing is uploaded until start() is
 * called, which the preview floater does only after the user confirmed the total cost.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLSOUNDSPLITUPLOAD_H
#define LL_LLSOUNDSPLITUPLOAD_H

#include "llaudiofiledecode.h"
#include "lluuid.h"

#include <memory>
#include <string>
#include <vector>

class LLSoundSplitUpload : public std::enable_shared_from_this<LLSoundSplitUpload>
{
public:
    struct Params
    {
        std::shared_ptr<const LLAudioClip> mClip;
        std::string mSourceFile;        // original file (only its extension is used)
        std::string mBaseName;          // parts become <base>_01, <base>_02, ...
        std::string mDescription;
        LLUUID      mDestFolder;
        S32         mCostPerClip = 0;
        U32         mNextOwnerPerms = 0;
        U32         mGroupPerms = 0;
        U32         mEveryonePerms = 0;
    };

    static std::shared_ptr<LLSoundSplitUpload> create(const Params& params);

    // Target part length from the SoundUploadSplitSeconds setting, kept safely below the limit.
    static double getSegmentSeconds();
    // Split plan the uploader would use for this clip (a single part when it fits).
    static std::vector<LLAudioSplitPart> planFor(const LLAudioClip& clip);

    size_t getPartCount() const { return mParts.size(); }
    S32    getTotalCost() const { return mParams.mCostPerClip * (S32)mParts.size(); }
    const std::string& getPartName(size_t i) const { return mParts[i].mName; }

    // Begin uploading part 1. Call only after the user confirmed the total cost.
    void start();

    // Called by the per-part upload info when its upload ends.
    void onPartFinished(size_t index, bool ok, const LLUUID& assetId, const std::string& reason);

private:
    enum class State { Pending, Uploading, Done, Failed };
    struct Part
    {
        LLAudioSplitPart mRange;
        std::string      mName;
        State            mState = State::Pending;
        LLUUID           mAssetId;
        std::string      mFailReason;
    };

    explicit LLSoundSplitUpload(const Params& params);

    void uploadPart(size_t index);
    void scheduleNext(size_t index);
    void reportFailure(size_t index);
    void reportDone();
    void retryRemaining();
    std::string buildUuidList() const;

    Params            mParams;
    std::vector<Part> mParts;
    size_t            mFadeFrames = 0;
};

#endif // LL_LLSOUNDSPLITUPLOAD_H
