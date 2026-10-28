/**
 * @file llsoundnotecardupload.cpp
 * @brief Bulk-upload WAV sounds and emit a music-player notecard.
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2024, Linden Research, Inc.
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

// <OTS> Sound -> notecard bulk upload (SoapStorm 525195d0af, dcabe78505).
// <GMDX> Adapted to the GMDX sound uploader: every file is decoded with
// llaudio_decode_file (WAV, Ogg Vorbis, Opus, MP3, FLAC) and uploaded from the
// decoded PCM, and a file over the 30 s limit is split into consecutive clips
// with the same plan, names and optional cut fades as LLSoundSplitUpload.

#include "llviewerprecompiledheaders.h"

#include "llsoundnotecardupload.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>

#include "lldbstrings.h"
#include "lldir.h"
#include "llnotecard.h"
#include "llnotificationsutil.h"
#include "llpermissionsflags.h"
#include "llstring.h"
#include "lltrans.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llappviewer.h"          // gDisconnected
#include "llaudiofiledecode.h"
#include "llcallbacklist.h"       // doAfterInterval()
#include "llfloaterperms.h"
#include "llinventorymodel.h"
#include "llsoundsplitupload.h"   // split plan (#807)
#include "llstatusbar.h"          // can_afford_transaction()
#include "llviewerassetupload.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"     // upload_new_resource()
#include "llviewermessage.h"      // suppress_inventory_auto_open_for_folder()
#include "llviewerregion.h"
#include "llvorbisencode.h"

namespace
{
    // -------------------------------------------------------------------------
    // Filename helpers
    // -------------------------------------------------------------------------

    // Parse the trailing run of digits in a base name (no extension).
    // "AuroraTheme_03" -> 3, "track 12" -> 12, "loop" -> -1 (no number).
    S32 parse_trailing_number(const std::string& stem)
    {
        std::string::size_type end = stem.size();
        std::string::size_type pos = end;
        while (pos > 0 && LLStringOps::isDigit(stem[pos - 1]))
        {
            --pos;
        }
        if (pos == end)
        {
            return -1; // no trailing digits
        }
        // Guard against absurdly long digit runs
        std::string digits = stem.substr(pos, std::min<std::string::size_type>(end - pos, 9));
        return (S32)atoi(digits.c_str());
    }

    // Longest common prefix of the base names, then strip any trailing
    // separators/digits/spaces so "AuroraTheme_01"/"AuroraTheme_02" -> "AuroraTheme".
    std::string derive_common_name(const std::vector<std::string>& stems)
    {
        if (stems.empty())
        {
            return std::string();
        }

        std::string prefix = stems[0];
        for (size_t i = 1; i < stems.size(); ++i)
        {
            const std::string& s = stems[i];
            std::string::size_type n = std::min(prefix.size(), s.size());
            std::string::size_type j = 0;
            while (j < n && prefix[j] == s[j])
            {
                ++j;
            }
            prefix.erase(j);
            if (prefix.empty())
            {
                break;
            }
        }

        // Trim trailing digits and common separator characters.
        while (!prefix.empty())
        {
            char c = prefix[prefix.size() - 1];
            if (LLStringOps::isDigit(c) || c == ' ' || c == '_' || c == '-' || c == '.')
            {
                prefix.erase(prefix.size() - 1);
            }
            else
            {
                break;
            }
        }

        LLStringUtil::trim(prefix);
        return prefix;
    }

    // Same clean-up the bulk uploader applies to asset names.
    std::string sanitize_asset_name(std::string name)
    {
        LLStringUtil::replaceNonstandardASCII(name, '?');
        LLStringUtil::replaceChar(name, '|', '?');
        LLStringUtil::stripNonprintable(name);
        LLStringUtil::trim(name);
        return name;
    }

    // One picked file, decoded.
    struct SoundEntry
    {
        std::string mFilename;   // full path
        std::string mName;       // base name without extension (asset name)
        S32         mSeq;        // trailing number, -1 if none
        size_t      mOrigIndex;  // stable tiebreak for unnumbered files
        std::shared_ptr<const LLAudioClip> mClip;
    };

    // <GMDX> One uploaded sound asset: a whole file, or one part of a split file.
    struct ClipUpload
    {
        std::string mFilename;   // source file (its extension picks the asset type)
        std::string mName;       // inventory name
        std::shared_ptr<const LLAudioClip> mClip;
        size_t      mStart;      // first frame
        size_t      mCount;      // frame count
        size_t      mFadeIn;
        size_t      mFadeOut;
        F32         mClipLength; // seconds
    };

    // Fade length at interior cuts of a split file, as LLSoundSplitUpload uses it
    // (off by default so plain cuts join seamlessly).
    size_t split_fade_frames()
    {
        if (!gSavedSettings.getBOOL("SoundUploadSplitFade"))
        {
            return 0;
        }
        const double ms = std::max(0.0, std::min(50.0, (double)gSavedSettings.getF32("SoundUploadSplitFadeMs")));
        return (size_t)std::llround(ms * (double)LLAUDIO_UPLOAD_SAMPLE_RATE / 1000.0);
    }

    // Expand the ordered files into the clips that get uploaded.
    std::vector<ClipUpload> plan_clips(const std::vector<SoundEntry>& entries, S32& split_files)
    {
        std::vector<ClipUpload> clips;
        split_files = 0;
        const size_t fade_frames = split_fade_frames();
        for (const SoundEntry& e : entries)
        {
            const std::vector<LLAudioSplitPart> plan = LLSoundSplitUpload::planFor(*e.mClip);
            if (plan.size() > 1)
            {
                ++split_files;
            }
            for (size_t i = 0; i < plan.size(); ++i)
            {
                ClipUpload c;
                c.mFilename = e.mFilename;
                c.mName = plan.size() > 1 ? llaudio_split_part_name(e.mName, i + 1, plan.size(), DB_INV_ITEM_NAME_STR_LEN)
                                          : e.mName;
                c.mClip = e.mClip;
                c.mStart = (size_t)plan[i].mStart;
                c.mCount = (size_t)plan[i].mCount;
                // Fade only at interior cuts: no fade-in on the first part, no fade-out on the last.
                const size_t fade = plan.size() > 1 ? std::min(fade_frames, c.mCount / 2) : 0;
                c.mFadeIn = i > 0 ? fade : 0;
                c.mFadeOut = i + 1 < plan.size() ? fade : 0;
                c.mClipLength = (F32)((double)c.mCount / (double)LLAUDIO_UPLOAD_SAMPLE_RATE);
                clips.push_back(c);
            }
        }
        return clips;
    }

    // -------------------------------------------------------------------------
    // Batch coordinator: owns one upload run and writes the notecard when done.
    // Kept alive by the per-sound upload infos (which hold a shared_ptr) and,
    // during finalization, by the inventory/asset callbacks it spawns.
    // -------------------------------------------------------------------------
    class LLSoundNotecardBatch : public std::enable_shared_from_this<LLSoundNotecardBatch>
    {
    public:
        LLSoundNotecardBatch(const std::string& notecard_name, const LLUUID& folder_id,
                             const std::vector<ClipUpload>& clips)
        :   mNotecardName(notecard_name)
        ,   mFolderId(folder_id)
        ,   mPending((S32)clips.size())
        ,   mFailed(0)
        {
            mSlots.reserve(clips.size());
            for (const ClipUpload& c : clips)
            {
                mSlots.push_back(Slot{ c.mClipLength, LLUUID::null, false });
            }
        }

        // Called from the upload coroutine (main thread) as each sound resolves.
        void reportResult(S32 index, const LLUUID& asset_id, bool success)
        {
            if (index >= 0 && index < (S32)mSlots.size())
            {
                mSlots[index].mAssetId = asset_id;
                mSlots[index].mSuccess = success;
            }
            if (!success)
            {
                ++mFailed;
            }
            if (--mPending <= 0)
            {
                finalize();
            }
        }

    private:
        struct Slot
        {
            F32    mClipLength;
            LLUUID mAssetId;
            bool   mSuccess;
        };

        void finalize()
        {
            // Gather successful sounds, preserving sequence order.
            std::vector<LLUUID> uuids;
            std::map<S32, S32> length_histogram; // key = length*10 (one decimal), value = count
            for (const Slot& slot : mSlots)
            {
                if (slot.mSuccess && slot.mAssetId.notNull())
                {
                    uuids.push_back(slot.mAssetId);
                    S32 key = (S32)ll_round(slot.mClipLength * 10.f);
                    length_histogram[key]++;
                }
            }

            if (uuids.empty())
            {
                LLNotificationsUtil::add("SoundNotecardAllFailed");
                return;
            }

            // Most common clip length; ties resolve to the longer length.
            S32 best_key = 0;
            S32 best_count = -1;
            for (const auto& it : length_histogram)
            {
                if (it.second > best_count || (it.second == best_count && it.first > best_key))
                {
                    best_count = it.second;
                    best_key = it.first;
                }
            }
            F32 modal_length = (F32)best_key / 10.f;

            // Build the notecard body: line 1 = modal length, then one UUID per line.
            std::ostringstream body;
            body << llformat("%.1f", modal_length);
            for (const LLUUID& id : uuids)
            {
                body << "\n" << id.asString();
            }

            // Wrap in the Linden notecard container format.
            LLNotecard nc(LLNotecard::MAX_SIZE);
            nc.setText(body.str());
            std::stringstream wrapped;
            nc.exportStream(wrapped);

            createNotecardItem(wrapped.str(), (S32)uuids.size());
        }

        void createNotecardItem(const std::string& asset_text, S32 sound_count)
        {
            auto self = shared_from_this();
            S32 failed = mFailed;
            LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
                [self, asset_text, sound_count, failed](const LLUUID& new_item_id)
                {
                    self->onNotecardItemCreated(new_item_id, asset_text, sound_count, failed);
                });

            // Keep the freshly created notecard from popping a notecard editor
            // window. Released in onNotecardItemCreated once the item has landed,
            // or after a minute if the sim never answers the create.
            suppress_inventory_auto_open_for_folder(mFolderId, true);
            const LLUUID folder_id = mFolderId;
            doAfterInterval([folder_id] { suppress_inventory_auto_open_for_folder(folder_id, false); }, 60.f);

            create_inventory_item(
                gAgentID,
                gAgentSessionID,
                mFolderId,
                LLTransactionID::tnull,
                mNotecardName,
                std::string(),
                LLAssetType::AT_NOTECARD,
                LLInventoryType::IT_NOTECARD,
                NO_INV_SUBTYPE,
                PERM_ALL,
                cb);
        }

        void onNotecardItemCreated(const LLUUID& item_id, const std::string& asset_text, S32 sound_count, S32 failed)
        {
            // The notecard has landed; stop suppressing auto-open for this folder.
            suppress_inventory_auto_open_for_folder(mFolderId, false);

            if (item_id.isNull())
            {
                LLNotificationsUtil::add("SoundNotecardCreateFailed");
                return;
            }

            LLViewerRegion* region = gAgent.getRegion();
            std::string url = region ? region->getCapability("UpdateNotecardAgentInventory") : std::string();
            if (url.empty())
            {
                LLNotificationsUtil::add("SoundNotecardCreateFailed");
                return;
            }

            std::string nc_name = mNotecardName;
            LLResourceUploadInfo::ptr_t info = std::make_shared<LLBufferedAssetUploadInfo>(
                item_id, LLAssetType::AT_NOTECARD, asset_text,
                [nc_name, sound_count, failed](LLUUID, LLUUID, LLUUID, LLSD)
                {
                    LLSD args;
                    args["NAME"] = nc_name;
                    args["COUNT"] = sound_count;
                    if (failed > 0)
                    {
                        args["FAILED"] = failed;
                        LLNotificationsUtil::add("SoundNotecardCreatedWithErrors", args);
                    }
                    else
                    {
                        LLNotificationsUtil::add("SoundNotecardCreated", args);
                    }
                },
                nullptr);

            LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
        }

        std::string       mNotecardName;
        LLUUID            mFolderId;
        std::vector<Slot> mSlots;
        S32               mPending;
        S32               mFailed;
    };

    // -------------------------------------------------------------------------
    // Per-sound upload that reports its result back to the batch.
    // -------------------------------------------------------------------------
    class LLSoundNotecardUploadInfo : public LLNewFileResourceUploadInfo
    {
    public:
        LLSoundNotecardUploadInfo(const ClipUpload& clip, S32 expected_cost, const LLUUID& folder_id,
                                  const std::shared_ptr<LLSoundNotecardBatch>& batch, S32 index, S32 count)
        :   LLNewFileResourceUploadInfo(
                clip.mFilename,
                clip.mName,
                clip.mName, // description
                0,
                LLFolderType::FT_NONE,
                LLInventoryType::IT_NONE,
                LLFloaterPerms::getNextOwnerPerms("Uploads"),
                LLFloaterPerms::getGroupPerms("Uploads"),
                LLFloaterPerms::getEveryonePerms("Uploads"),
                expected_cost,
                folder_id,
                false) // don't pop a preview floater per clip
        ,   mBatch(batch)
        ,   mIndex(index)
        ,   mCount(count)
        {
            // <GMDX> Encode from the decoded PCM, like the sound preview and split uploads.
            setAudioClip(clip.mClip, clip.mStart, clip.mCount, clip.mFadeIn, clip.mFadeOut);
        }

        // <GMDX> Shown in the modal "Uploading..." dialog.
        std::string getDisplayName() const override
        {
            LLStringUtil::format_map_t args;
            args["[INDEX]"] = llformat("%d", mIndex + 1);
            args["[COUNT]"] = llformat("%d", mCount);
            args["[NAME]"] = getName();
            return LLTrans::getString("SoundSplitProgress", args);
        }

        LLUUID finishUpload(LLSD& result) override
        {
            LLUUID item_id = LLNewFileResourceUploadInfo::finishUpload(result);
            if (mBatch)
            {
                const LLUUID asset_id = result["new_asset"].asUUID();
                mBatch->reportResult(mIndex, asset_id, asset_id.notNull());
                mBatch.reset();
            }
            return item_id;
        }

        bool failedUpload(LLSD& result, std::string& reason) override
        {
            if (mBatch)
            {
                mBatch->reportResult(mIndex, LLUUID::null, false);
                mBatch.reset();
            }
            return false; // let the standard upload-error notification fire
        }

    private:
        std::shared_ptr<LLSoundNotecardBatch> mBatch;
        S32                                   mIndex;
        S32                                   mCount;
    };

    // <GMDX> Why uploads cannot start right now, or an empty string. Without the
    // capability upload_new_resource falls back to the legacy asset path, which
    // never reports back to the batch.
    std::string upload_blocked_reason()
    {
        if (gDisconnected)
        {
            return LLTrans::getString("SoundSplitDisconnected");
        }
        if (gAgent.getRegionCapability("NewFileAgentInventory").empty())
        {
            return LLTrans::getString("SoundSplitNoCap");
        }
        return std::string();
    }

    // -------------------------------------------------------------------------
    // Kick off the uploads once the destination subfolder exists.
    // -------------------------------------------------------------------------
    void launch_uploads(const std::vector<ClipUpload>& clips, const std::string& notecard_name,
                        S32 per_sound_cost, const LLUUID& subfolder_id)
    {
        if (subfolder_id.isNull())
        {
            LLNotificationsUtil::add("SoundNotecardFolderFailed");
            return;
        }
        const std::string blocked = upload_blocked_reason();
        if (!blocked.empty())
        {
            LLNotificationsUtil::add("SoundNotecardCannotUpload", LLSD().with("REASON", blocked));
            return;
        }

        auto batch = std::make_shared<LLSoundNotecardBatch>(notecard_name, subfolder_id, clips);

        for (size_t i = 0; i < clips.size(); ++i)
        {
            LLResourceUploadInfo::ptr_t info = std::make_shared<LLSoundNotecardUploadInfo>(
                clips[i], per_sound_cost, subfolder_id, batch, (S32)i, (S32)clips.size());
            upload_new_resource(info);
        }
    }

    // Confirmation-dialog response handler.
    void on_confirm_upload(const LLSD& notification, const LLSD& response,
                           std::vector<ClipUpload> clips, std::string notecard_name, S32 per_sound_cost)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) != 0)
        {
            return; // cancelled
        }

        const std::string blocked = upload_blocked_reason();
        if (!blocked.empty())
        {
            LLNotificationsUtil::add("SoundNotecardCannotUpload", LLSD().with("REASON", blocked));
            return;
        }

        const LLUUID sounds_folder = gInventory.findCategoryUUIDForType(LLFolderType::FT_SOUND);
        if (sounds_folder.isNull())
        {
            LLNotificationsUtil::add("SoundNotecardFolderFailed");
            return;
        }

        gInventory.createNewCategory(
            sounds_folder,
            LLFolderType::FT_NONE,
            notecard_name,
            [clips, notecard_name, per_sound_cost](const LLUUID& new_cat_id)
            {
                launch_uploads(clips, notecard_name, per_sound_cost, new_cat_id);
            });
    }
} // anonymous namespace

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------
void start_sound_notecard_upload(const std::vector<std::string>& filenames)
{
    std::vector<SoundEntry> entries;
    std::vector<std::string> stems;
    S32 skipped = 0;

    for (size_t i = 0; i < filenames.size(); ++i)
    {
        const std::string& filename = filenames[i];

        // <GMDX> Same validation as the single sound upload: WAVs are pre-qualified
        // (with the split cap as the length limit), then every format is decoded and
        // the decoded length is what counts.
        std::string ext = gDirUtilp->getExtension(filename);
        LLStringUtil::toLower(ext);
        std::string error_msg;
        if (ext == "wav" && check_for_invalid_wav_formats(filename, error_msg, (F32)LLAUDIO_DECODE_MAX_SECONDS))
        {
            LL_INFOS() << "Skipping " << filename << " for notecard upload: " << error_msg << LL_ENDL;
            ++skipped;
            continue;
        }

        std::shared_ptr<LLAudioClip> clip = std::make_shared<LLAudioClip>();
        if (llaudio_decode_file(filename, *clip, error_msg) != LLAUDIO_DEC_OK || clip->getFrames() == 0)
        {
            LL_INFOS() << "Skipping " << filename << " for notecard upload: " << error_msg << LL_ENDL;
            ++skipped;
            continue;
        }
        // </GMDX>

        std::string stem = sanitize_asset_name(gDirUtilp->getBaseFileName(filename, true));
        if (stem.empty())
        {
            stem = "Sound";
        }

        SoundEntry e;
        e.mFilename   = filename;
        e.mName       = stem;
        e.mSeq        = parse_trailing_number(stem);
        e.mOrigIndex  = i;
        e.mClip       = clip;
        entries.push_back(e);
        stems.push_back(stem);
    }

    if (entries.empty())
    {
        LLNotificationsUtil::add("SoundNotecardNoFiles");
        return;
    }

    // Order by trailing number; unnumbered files keep their pick order, after numbered ones.
    std::sort(entries.begin(), entries.end(), [](const SoundEntry& a, const SoundEntry& b)
        {
            bool a_num = a.mSeq >= 0;
            bool b_num = b.mSeq >= 0;
            if (a_num != b_num)
            {
                return a_num; // numbered files sort first
            }
            if (a_num && a.mSeq != b.mSeq)
            {
                return a.mSeq < b.mSeq;
            }
            return a.mOrigIndex < b.mOrigIndex;
        });

    std::string notecard_name = derive_common_name(stems);
    if (notecard_name.empty())
    {
        notecard_name = "Sound List";
    }

    // <GMDX> Long files become several clips, in order.
    S32 split_files = 0;
    std::vector<ClipUpload> clips = plan_clips(entries, split_files);

    // Cost + balance check.
    S32 per_sound_cost = LLAgentBenefitsMgr::current().getSoundUploadCost();
    S32 total_cost = per_sound_cost * (S32)clips.size();
    if (!can_afford_transaction(total_cost))
    {
        LLSD args;
        args["COST"] = llformat("%d", total_cost);
        LLNotificationsUtil::add("ErrorCannotAffordUpload", args);
        return;
    }

    // <GMDX> One confirmation; the optional lines (skipped files, split files) go in [DETAILS].
    std::string details;
    if (split_files > 0)
    {
        LLStringUtil::format_map_t sargs;
        sargs["[FILES]"] = llformat("%d", split_files);
        sargs["[SECONDS]"] = llformat("%.1f", LLSoundSplitUpload::getSegmentSeconds());
        details += "\n\n" + LLTrans::getString("SoundNotecardSplitInfo", sargs);
    }
    if (skipped > 0)
    {
        LLStringUtil::format_map_t sargs;
        sargs["[SKIPPED]"] = llformat("%d", skipped);
        details += "\n\n" + LLTrans::getString("SoundNotecardSkippedInfo", sargs);
    }

    LLSD args;
    args["COUNT"] = (S32)clips.size();
    args["COST"] = total_cost;
    args["NAME"] = notecard_name;
    args["DETAILS"] = details;

    // ponytail: every decoded file stays in memory until its clips have uploaded
    // (mono 16-bit, ~5 MB per minute); decode lazily per clip if huge batches matter.
    LLNotificationsUtil::add(
        "SoundNotecardConfirm",
        args,
        LLSD(),
        boost::bind(&on_confirm_upload, _1, _2, clips, notecard_name, per_sound_cost));
}
