/**
 * @file llavatariconloader.cpp
 * @brief Shared avatar profile picture loading
 *
 * $LicenseInfo:firstyear=2013&license=viewerlgpl$
 * Copyright (c) 2013 Ansariel Hiller @ Second Life
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include <algorithm>

#include "llavatariconloader.h"

#include "llagent.h"
#include "llavatariconctrl.h"
#include "llframetimer.h"
#include "llui.h"
#include "lltexturedata.h"
#include "llviewerregion.h"
#include "llviewertexture.h"

namespace
{
    bool imageMissing(const LLUIImagePtr& image)
    {
        const auto* texture = dynamic_cast<const LLViewerFetchedTexture*>(image->getImage().get());
        return texture && texture->isMissingAsset();
    }

    bool imageReady(const LLUIImagePtr& image)
    {
        const LLTextureData* data = image->getImage()->getTextureData();
        return data && data->getDiscardLevel() >= 0;
    }
}

LLAvatarIconLoader::LLAvatarIconLoader()
    : LLEventTimer(1.f)
{
}

boost::signals2::connection LLAvatarIconLoader::setUpdateCallback(const update_signal_t::slot_type& callback)
{
    return mChanged.connect(callback);
}

LLUIImagePtr LLAvatarIconLoader::getIcon(const LLUUID& avatar_id, S32 size, bool visible, bool profile_service)
{
    LLUUID icon_id;
    if (request(avatar_id, icon_id, visible, profile_service) != Result::Known || icon_id.isNull())
    {
        return LLUIImagePtr();
    }

    return loadIcon(icon_id, size, visible);
}

LLUIImagePtr LLAvatarIconLoader::loadIcon(const LLUUID& icon_id, S32 size, bool request)
{
    auto it = mImages.find(icon_id);
    if (it == mImages.end())
    {
        if (!request)
        {
            return LLUIImagePtr();
        }
        LLUIImagePtr image = LLUI::getUIImageByID(icon_id, LLGLTexture::BOOST_ICON);
        if (!image || !image->getImage())
        {
            return LLUIImagePtr();
        }
        it = mImages.emplace(icon_id, Image{ image, LLTimer::getTotalSeconds(), false }).first;
    }
    if (request)
    {
        it->second.lastUsed = LLTimer::getTotalSeconds();
        const S32 draw_size = llmax(size, 32);
        it->second.image->getImage()->setKnownDrawSize(draw_size, draw_size);
    }
    return imageReady(it->second.image) ? it->second.image : LLUIImagePtr();
}

namespace
{
    constexpr S32 AVATAR_ICON_BACKGROUND_LIMIT = 2;
    constexpr S32 AVATAR_ICON_MAX_IN_FLIGHT = 8;
    constexpr S32 AVATAR_ICON_MAX_PER_PASS = 4;
    constexpr F64 AVATAR_ICON_TIMEOUT_SECS = 10.0;   // frees the in-flight slot
    constexpr F64 AVATAR_ICON_BACKOFF_SECS = 15.0;   // doubled per failed attempt
    constexpr S32 AVATAR_ICON_MAX_ATTEMPTS = 4;      // then give up (silhouette) for AVATAR_ICON_GIVEUP_SECS
    constexpr F64 AVATAR_ICON_LATE_SECS = 90.0;      // a timed-out request is still listened for this long
    constexpr F64 AVATAR_ICON_GIVEUP_SECS = 300.0;   // after this a given-up avatar is tried again
    constexpr F64 AVATAR_ICON_STATS_SECS = 30.0;
}

bool LLAvatarIconLoader::prefetchIcon(const LLUUID& avatar_id, S32 size)
{
    LLUUID icon_id;
    const Result result = request(avatar_id, icon_id, false, true);
    if (result == Result::GaveUp || (result == Result::Known && icon_id.isNull()))
    {
        return true;
    }
    auto image = mImages.find(icon_id);
    if (image != mImages.end())
    {
        return imageReady(image->second.image) || imageMissing(image->second.image);
    }
    if (mPending.count(avatar_id) || mBackoff.count(avatar_id) || mPending.size() >= AVATAR_ICON_BACKGROUND_LIMIT)
    {
        return false;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (now - mBackgroundTime >= 1.0)
    {
        mBackgroundTime = now;
        mBackgroundStarts = 0;
    }
    if (mBackgroundStarts >= AVATAR_ICON_BACKGROUND_LIMIT
        || std::count_if(mImages.begin(), mImages.end(), [](const auto& entry)
            { return !imageReady(entry.second.image) && !imageMissing(entry.second.image); }) >= AVATAR_ICON_BACKGROUND_LIMIT)
    {
        return false;
    }
    ++mBackgroundStarts;
    if (result == Result::Known)
    {
        return bool(loadIcon(icon_id, size, true));
    }
    request(avatar_id, icon_id, true, true);
    return false;
}

bool LLAvatarIconLoader::tick()
{
    const F64 now = LLTimer::getTotalSeconds();

    for (auto it = mPending.begin(); it != mPending.end();)
    {
        if (now - it->second.time > AVATAR_ICON_TIMEOUT_SECS)
        {
            mLate[it->first] = { now, it->second.type };
            const S32 attempts = mAttempts[it->first];
            mBackoff[it->first] = now + AVATAR_ICON_BACKOFF_SECS * (F64)(1 << llmin(attempts - 1, 4));
            ++mTimedOut;
            it = mPending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = mLate.begin(); it != mLate.end();)
    {
        if (now - it->second.time > AVATAR_ICON_LATE_SECS)
        {
            LLAvatarPropertiesProcessor::getInstance()->removeObserver(it->first, this);
            it = mLate.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = mBackoff.begin(); it != mBackoff.end();)
    {
        it = (it->second < now) ? mBackoff.erase(it) : std::next(it);
    }

    bool changed = false;
    U32 loading = 0;
    for (auto it = mImages.begin(); it != mImages.end();)
    {
        if (now - it->second.lastUsed > AVATAR_ICON_LATE_SECS)
        {
            it = mImages.erase(it);
            continue;
        }
        const bool ready = imageReady(it->second.image);
        loading += !ready;
        changed |= ready != it->second.ready;
        it->second.ready = ready;
        ++it;
    }
    for (auto it = mLastUsed.begin(); it != mLastUsed.end();)
    {
        if (now - it->second > AVATAR_ICON_GIVEUP_SECS
            && !mPending.count(it->first) && !mLate.count(it->first))
        {
            mNullChecked.erase(it->first);
            mAttempts.erase(it->first);
            mBackoff.erase(it->first);
            mGaveUpUntil.erase(it->first);
            it = mLastUsed.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (changed)
    {
        mChanged();
    }

    if (now - mLastStats > AVATAR_ICON_STATS_SECS)
    {
        mLastStats = now;
        LL_INFOS("AvatarIcons") << "Profile pictures: requested=" << mRequested << " replied=" << mReplied
            << " timed_out=" << mTimedOut << " no_picture=" << mNoPicture << " gave_up=" << mGaveUp
            << " in_flight=" << mPending.size() << " backoff=" << mBackoff.size()
            << " images=" << mImages.size() << " loading=" << loading << LL_ENDL;
    }
    return false;
}

LLAvatarIconLoader::Result LLAvatarIconLoader::request(const LLUUID& avatar_id, LLUUID& icon_id, bool may_request, bool profile_service)
{
    if (avatar_id.isNull())
    {
        return Result::Unknown;
    }
    if (mRequestFrame != LLFrameTimer::getFrameCount())
    {
        mRequestFrame = LLFrameTimer::getFrameCount();
        mRequestsThisPass = 0;
    }
    if (may_request)
    {
        mLastUsed[avatar_id] = LLTimer::getTotalSeconds();
    }
    const LLViewerRegion* region = gAgent.getRegion();
    const EAvatarProcessorType request_type =
        profile_service && region && region->capabilitiesReceived() && region->isCapabilityAvailable("AgentProfile")
        ? APT_PROPERTIES : APT_PROPERTIES_LEGACY;

    if (LLUUID* cached = LLAvatarIconIDCache::getInstance()->get(avatar_id))
    {
        icon_id = *cached;
        auto checked = mNullChecked.find(avatar_id);
        if (cached->notNull() || !profile_service
            || (checked != mNullChecked.end()
                && (checked->second == APT_PROPERTIES || checked->second == request_type)))
        {
            return Result::Known;
        }
    }
    else
    {
        mNullChecked.erase(avatar_id);
    }
    const Result waiting = Result::Unknown;
    const F64 now = LLTimer::getTotalSeconds();
    auto att = mAttempts.find(avatar_id);
    if (att != mAttempts.end() && att->second >= AVATAR_ICON_MAX_ATTEMPTS && !mPending.count(avatar_id))
    {
        auto until = mGaveUpUntil.find(avatar_id);
        if (until != mGaveUpUntil.end() && now < until->second)
        {
            return Result::GaveUp;
        }
        mAttempts.erase(att);
        if (until != mGaveUpUntil.end())
        {
            mGaveUpUntil.erase(until);
        }
    }
    if (!may_request || mPending.count(avatar_id) || mBackoff.count(avatar_id)
        || (S32)mPending.size() >= AVATAR_ICON_MAX_IN_FLIGHT || mRequestsThisPass >= AVATAR_ICON_MAX_PER_PASS)
    {
        return waiting;
    }
    ++mRequestsThisPass;
    ++mRequested;
    if (++mAttempts[avatar_id] >= AVATAR_ICON_MAX_ATTEMPTS)
    {
        ++mGaveUp;
        mGaveUpUntil[avatar_id] = now + AVATAR_ICON_GIVEUP_SECS;
    }
    mLate.erase(avatar_id);
    mPending[avatar_id] = { now, request_type };
    LLAvatarPropertiesProcessor* app = LLAvatarPropertiesProcessor::getInstance();
    app->addObserver(avatar_id, this);
    if (request_type == APT_PROPERTIES)
    {
        app->sendAvatarPropertiesRequest(avatar_id);
    }
    else
    {
        app->sendAvatarLegacyPropertiesRequest(avatar_id);
    }
    return waiting;
}

LLAvatarIconLoader::~LLAvatarIconLoader()
{
    if (LLAvatarPropertiesProcessor::instanceExists())
    {
        LLAvatarPropertiesProcessor* app = LLAvatarPropertiesProcessor::getInstance();
        for (const auto& pending : mPending)
        {
            app->removeObserver(pending.first, this);
        }
        for (const auto& late : mLate)
        {
            app->removeObserver(late.first, this);
        }
    }
}

void LLAvatarIconLoader::processProperties(void* data, EAvatarProcessorType type)
{
    if (!data)
    {
        return;
    }
    LLUUID avatar_id;
    LLUUID image_id;
    if (type == APT_PROPERTIES_LEGACY)
    {
        const LLAvatarLegacyData* d = static_cast<LLAvatarLegacyData*>(data);
        avatar_id = d->avatar_id;
        image_id = d->image_id;
    }
    else if (type == APT_PROPERTIES)
    {
        const LLAvatarData* d = static_cast<LLAvatarData*>(data);
        avatar_id = d->avatar_id;
        image_id = d->image_id;
    }
    else
    {
        return;
    }

    auto it = mPending.find(avatar_id);
    auto late = mLate.find(avatar_id);
    if (it == mPending.end() && late == mLate.end())
    {
        return;
    }
    const Request& request = it != mPending.end() ? it->second : late->second;
    if (type != request.type)
    {
        return;
    }
    if (image_id.isNull())
    {
        mNullChecked[avatar_id] = type;
    }
    else
    {
        mNullChecked.erase(avatar_id);
    }
    if (it != mPending.end())
    {
        mPending.erase(it);
    }
    else
    {
        mLate.erase(late);
    }
    mAttempts.erase(avatar_id);
    mGaveUpUntil.erase(avatar_id);
    mBackoff.erase(avatar_id);
    ++mReplied;
    if (image_id.isNull())
    {
        ++mNoPicture;
    }
    LLAvatarPropertiesProcessor::getInstance()->removeObserver(avatar_id, this);
    LLAvatarIconIDCache::getInstance()->add(avatar_id, image_id);
    mChanged();
}
