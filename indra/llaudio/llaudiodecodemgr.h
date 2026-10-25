/**
 * @file llaudiodecodemgr.h
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#ifndef LL_LLAUDIODECODEMGR_H
#define LL_LLAUDIODECODEMGR_H

#include "stdtypes.h"

#include "lluuid.h"

#include "llassettype.h"
#include "llframetimer.h"
#include "llsingleton.h"

template<class T> class LLPointer;
class LLVorbisDecodeState;

class LLAudioDecodeMgr : public LLSingleton<LLAudioDecodeMgr>
{
    LLSINGLETON(LLAudioDecodeMgr);
    ~LLAudioDecodeMgr();
public:
    void processQueue();
    // urgent: put it first in line (a sound wanted now, not a preload). #891
    bool addDecodeRequest(const LLUUID &uuid, bool urgent = false);
    void addAudioRequest(const LLUUID &uuid);
    // #883 MemTrend: sounds waiting to decode / decodes holding a slot (in flight or
    // writing the decoded file).
    size_t queuedCount() const;
    size_t inFlightCount() const;
    // #891: queued or in flight (the sound data sweep must not free it meanwhile).
    bool isPending(const LLUUID& uuid) const;

protected:
    class Impl;
    Impl* mImpl;
};

#endif
