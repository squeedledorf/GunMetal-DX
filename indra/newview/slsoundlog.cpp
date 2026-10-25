/**
 * @file slsoundlog.cpp
 * @brief Bounded log of recently played sounds, feeding the Sound Explorer (#654)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slsoundlog.h)
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slsoundlog.h"
#include "lldate.h"

#include "llaudioengine.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llworld.h"

namespace
{
    constexpr F32 CHANGE_THROTTLE_SECONDS = 0.25f;
    constexpr U32 HARD_MIN_ENTRIES = 16;
}

SLSoundLog::SLSoundLog()
{
    mThrottle.start();
}

void SLSoundLog::clear()
{
    mEntries.clear();
    mIndex.clear();
    mPending = false;
    mChangedSignal();
}

SLSoundLogEntry& SLSoundLog::touch(const LLUUID& sound_id, const LLUUID& source_id, bool& is_new)
{
    const F64 now = LLDate::now().secondsSinceEpoch();
    Key key{ sound_id, source_id };
    auto it = mIndex.find(key);
    if (it != mIndex.end())
    {
        is_new = false;
        // Replayed: move to the front (most recent).
        mEntries.splice(mEntries.begin(), mEntries, it->second);
        SLSoundLogEntry& e = *it->second;
        e.mLastPlayed = now;
        ++e.mPlayCount;
        return e;
    }

    is_new = true;
    mEntries.emplace_front();
    SLSoundLogEntry& e = mEntries.front();
    e.mSoundID = sound_id;
    e.mSourceID = source_id;
    e.mFirstPlayed = e.mLastPlayed = now;
    e.mPlayCount = 1;
    mIndex.emplace(key, mEntries.begin());
    evictOverflow();
    return mEntries.front();
}

void SLSoundLog::evictOverflow()
{
    const U32 cap = std::max<U32>(HARD_MIN_ENTRIES, gSavedSettings.getU32("SoundLogMaxEntries"));
    while (mEntries.size() > cap)
    {
        const SLSoundLogEntry& old = mEntries.back();
        mIndex.erase(Key{ old.mSoundID, old.mSourceID });
        mEntries.pop_back();
    }
}

void SLSoundLog::recordTriggered(const LLUUID& sound_id, const LLUUID& source_id, const LLUUID& owner_id,
                                 const LLVector3d& pos_global, const LLUUID& audio_source_id, bool collision)
{
    if (sound_id.isNull())
    {
        return;
    }
    bool is_new = false;
    SLSoundLogEntry& e = touch(sound_id, source_id, is_new);
    e.mOwnerID = owner_id;
    e.mPosGlobal = pos_global;
    e.mAudioSourceID = audio_source_id;
    e.mType = collision ? SLSoundLogEntry::TYPE_COLLISION : SLSoundLogEntry::TYPE_TRIGGERED;
    e.mLoop = false;
    if (LLViewerRegion* regionp = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        e.mRegion = regionp->getName();
    }
    notifyChanged();
}

void SLSoundLog::recordAttached(const LLUUID& sound_id, LLViewerObject* objectp, const LLUUID& owner_id, bool loop)
{
    if (sound_id.isNull() || !objectp)
    {
        return;
    }
    bool is_new = false;
    SLSoundLogEntry& e = touch(sound_id, objectp->getID(), is_new);
    e.mOwnerID = owner_id;
    e.mPosGlobal = objectp->getPositionGlobal();
    e.mAudioSourceID = objectp->getID(); // attached sources are keyed by object id in the engine
    e.mType = SLSoundLogEntry::TYPE_ATTACHED;
    e.mLoop = loop;
    if (LLViewerRegion* regionp = objectp->getRegion())
    {
        e.mRegion = regionp->getName();
    }
    notifyChanged();
}

// static
bool SLSoundLog::isPlaying(const SLSoundLogEntry& entry)
{
    if (!gAudiop || entry.mAudioSourceID.isNull())
    {
        return false;
    }
    LLAudioSource* sourcep = gAudiop->findAudioSource(entry.mAudioSourceID);
    if (!sourcep || sourcep->isMuted() || !sourcep->isChannelPlaying())
    {
        return false;
    }
    // An object's attached source can have moved on to a different sound.
    LLAudioData* datap = sourcep->getCurrentData();
    return datap && datap->getID() == entry.mSoundID;
}

void SLSoundLog::notifyChanged()
{
    if (mThrottle.getElapsedTimeF32() >= CHANGE_THROTTLE_SECONDS)
    {
        mThrottle.reset();
        mPending = false;
        mChangedSignal();
    }
    else
    {
        mPending = true;
    }
}

void SLSoundLog::flushPending()
{
    if (mPending && mThrottle.getElapsedTimeF32() >= CHANGE_THROTTLE_SECONDS)
    {
        mThrottle.reset();
        mPending = false;
        mChangedSignal();
    }
}
