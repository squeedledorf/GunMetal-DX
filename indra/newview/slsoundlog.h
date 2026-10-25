/**
 * @file slsoundlog.h
 * @brief Bounded log of recently played sounds, feeding the Sound Explorer (#654)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
 * $/LicenseInfo$
 */

#ifndef SL_SOUNDLOG_H
#define SL_SOUNDLOG_H

#include "llsingleton.h"
#include "lluuid.h"
#include "v3dmath.h"
#include "llframetimer.h"

#include <list>
#include <string>
#include <unordered_map>
#include <boost/signals2.hpp>

class LLViewerObject;

struct SLSoundLogEntry
{
    enum EType
    {
        TYPE_ATTACHED = 0,  // looped / attached sound set on an object
        TYPE_TRIGGERED,     // one-shot SoundTrigger (llTriggerSound, gestures)
        TYPE_COLLISION,     // collision / footstep sounds
    };

    LLUUID      mSoundID;
    LLUUID      mSourceID;      // source object (or avatar) id; null if unknown
    LLUUID      mOwnerID;
    LLUUID      mAudioSourceID; // LLAudioSource id in the engine (to ask "still playing?")
    std::string mRegion;
    LLVector3d  mPosGlobal;
    F64         mFirstPlayed = 0.0;     // unix seconds
    F64         mLastPlayed = 0.0;      // unix seconds
    U32         mPlayCount = 0;
    EType       mType = TYPE_TRIGGERED;
    bool        mLoop = false;
};

// Records every sound the viewer starts, bounded by the SoundLogMaxEntries setting with
// least-recently-played eviction (the list is kept in most-recent-first order, so eviction
// is O(1) from the tail and a replay moves the entry to the head).
class SLSoundLog : public LLSingleton<SLSoundLog>
{
    LLSINGLETON(SLSoundLog);

public:
    using entry_list_t = std::list<SLSoundLogEntry>;
    using change_signal_t = boost::signals2::signal<void()>;

    void recordTriggered(const LLUUID& sound_id, const LLUUID& source_id, const LLUUID& owner_id,
                         const LLVector3d& pos_global, const LLUUID& audio_source_id, bool collision);
    void recordAttached(const LLUUID& sound_id, LLViewerObject* objectp, const LLUUID& owner_id, bool loop);

    // Most recently played first.
    const entry_list_t& getEntries() const { return mEntries; }
    size_t size() const { return mEntries.size(); }
    void clear();

    // Is this entry's sound audibly playing right now?
    static bool isPlaying(const SLSoundLogEntry& entry);

    // Fired at most ~4 times per second no matter how many sounds start.
    boost::signals2::connection setChangeCallback(const change_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }
    // Fire a throttled change that was held back (call from a UI tick).
    void flushPending();

private:
    struct Key
    {
        LLUUID mSound, mSource;
        bool operator==(const Key& o) const { return mSound == o.mSound && mSource == o.mSource; }
    };
    struct KeyHash
    {
        size_t operator()(const Key& k) const { return std::hash<LLUUID>()(k.mSound) * 31u + std::hash<LLUUID>()(k.mSource); }
    };

    SLSoundLogEntry& touch(const LLUUID& sound_id, const LLUUID& source_id, bool& is_new);
    void evictOverflow();
    void notifyChanged();

    entry_list_t mEntries;
    std::unordered_map<Key, entry_list_t::iterator, KeyHash> mIndex;
    change_signal_t mChangedSignal;
    LLFrameTimer mThrottle;
    bool mPending = false;
};

#endif // SL_SOUNDLOG_H
