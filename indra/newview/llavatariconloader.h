/**
 * @file llavatariconloader.h
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

#ifndef LL_LLAVATARICONLOADER_H
#define LL_LLAVATARICONLOADER_H

#include "llavatarpropertiesprocessor.h"
#include "lleventtimer.h"
#include "llsingleton.h"
#include "lluiimage.h"

#include <boost/signals2.hpp>
#include <map>

class LLAvatarIconLoader : public LLSingleton<LLAvatarIconLoader>,
                           public LLAvatarPropertiesObserver,
                           public LLEventTimer
{
    LLSINGLETON(LLAvatarIconLoader);
    ~LLAvatarIconLoader() override;

public:
    typedef boost::signals2::signal<void()> update_signal_t;

    boost::signals2::connection setUpdateCallback(const update_signal_t::slot_type& callback);
    LLUIImagePtr getIcon(const LLUUID& avatar_id, S32 size, bool visible, bool profile_service);
    bool prefetchIcon(const LLUUID& avatar_id, S32 size);
    void processProperties(void* data, EAvatarProcessorType type) override;
    bool tick() override;

private:
    LLUIImagePtr loadIcon(const LLUUID& icon_id, S32 size, bool request);
    enum class Result { Unknown, Known, GaveUp };
    Result request(const LLUUID& avatar_id, LLUUID& icon_id, bool may_request, bool profile_service);

    struct Request
    {
        F64 time;
        EAvatarProcessorType type;
    };

    struct Image
    {
        LLUIImagePtr image;
        F64 lastUsed;
        bool ready;
    };

    std::map<LLUUID, Request> mPending;
    std::map<LLUUID, Request> mLate;
    std::map<LLUUID, F64> mBackoff;
    std::map<LLUUID, S32> mAttempts;
    std::map<LLUUID, EAvatarProcessorType> mNullChecked;
    std::map<LLUUID, F64> mGaveUpUntil;
    std::map<LLUUID, F64> mLastUsed;
    std::map<LLUUID, Image> mImages;
    U32 mRequestFrame = 0;
    S32 mRequestsThisPass = 0;
    U32 mRequested = 0, mReplied = 0, mTimedOut = 0, mNoPicture = 0, mGaveUp = 0;
    F64 mLastStats = 0.0;
    F64 mBackgroundTime = 0.0;
    S32 mBackgroundStarts = 0;
    update_signal_t mChanged;
};

#endif
