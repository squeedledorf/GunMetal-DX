 /**
 * @file audioengine.cpp
 * @brief implementation of LLAudioEngine class abstracting the Open
 * AL audio support
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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

#include "linden_common.h"

#include "llaudioengine.h"
#include "llstreamingaudio.h"

#include "llerror.h"
#include "llmath.h"

#include "sound_ids.h"  // temporary hack for min/max distances

#include "llfilesystem.h"
#include "lldir.h"
#include "llaudiodecodemgr.h"
#include "llsoundcache.h"
#include "llassetstorage.h"

#include "llstring.h"

#include <sstream>
#include <unordered_set>


// necessary for grabbing sounds from sim (implemented in viewer)
extern void request_sound(const LLUUID &sound_guid);

LLAudioEngine* gAudiop = NULL;


//
// LLAudioEngine implementation
//


LLAudioEngine::LLAudioEngine()
{
    setDefaults();
}


LLAudioEngine::~LLAudioEngine()
{
}

LLStreamingAudioInterface* LLAudioEngine::getStreamingAudioImpl()
{
    return mStreamingAudioImpl;
}

void LLAudioEngine::setStreamingAudioImpl(LLStreamingAudioInterface *impl)
{
    mStreamingAudioImpl = impl;
}

void LLAudioEngine::setDefaults()
{
    mMaxWindGain = 1.f;

    mListenerp = NULL;

    mMuted = false;
    mUserData = NULL;

    mLastStatus = 0;

    mEnableWind = false;

    mChannels.fill(nullptr);
    mBuffers.fill(nullptr);

    mMasterGain = 1.f;
    // Setting mInternalGain to an out of range value fixes the issue reported in STORM-830.
    // There is an edge case in setMasterGain during startup which prevents setInternalGain from
    // being called if the master volume setting and mInternalGain both equal 0, so using -1 forces
    // the if statement in setMasterGain to execute when the viewer starts up.
    mInternalGain = -1.f;
    mNextWindUpdate = 0.f;

    mStreamingAudioImpl = NULL;

    for (U32 i = 0; i < LLAudioEngine::AUDIO_TYPE_COUNT; i++)
        mSecondaryGain[i] = 1.0f;
}


bool LLAudioEngine::init(void* userdata, const std::string &app_title)
{
    setDefaults();

    mUserData = userdata;

    allocateListener();

    LL_INFOS("AudioEngine") << "LLAudioEngine::init() AudioEngine successfully initialized" << LL_ENDL;

    return true;
}


void LLAudioEngine::shutdown()
{
    // Clean up wind source
    cleanupWind();

    // Clean up audio sources
    for (source_map::value_type& src_pair : mAllSources)
    {
        delete src_pair.second;
    }


    // Clean up audio data
    for (data_map::value_type& data_pair : mAllData)
    {
        delete data_pair.second;
    }


    // Clean up channels
    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        delete mChannels[i];
        mChannels[i] = NULL;
    }

    // Clean up buffers
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        delete mBuffers[i];
        mBuffers[i] = NULL;
    }
}


// virtual
void LLAudioEngine::startInternetStream(const std::string& url)
{
    if (mStreamingAudioImpl)
        mStreamingAudioImpl->start(url);
}


// virtual
void LLAudioEngine::stopInternetStream()
{
    if (mStreamingAudioImpl)
        mStreamingAudioImpl->stop();
}

// virtual
void LLAudioEngine::pauseInternetStream(S32 pause)
{
    if (mStreamingAudioImpl)
        mStreamingAudioImpl->pause(pause);
}

// virtual
void LLAudioEngine::updateInternetStream()
{
    if (mStreamingAudioImpl)
        mStreamingAudioImpl->update();
}

// virtual
LLAudioEngine::LLAudioPlayState LLAudioEngine::isInternetStreamPlaying()
{
    if (mStreamingAudioImpl)
        return (LLAudioEngine::LLAudioPlayState) mStreamingAudioImpl->isPlaying();

    return LLAudioEngine::AUDIO_STOPPED; // Stopped
}


// virtual
void LLAudioEngine::setInternetStreamGain(F32 vol)
{
    if (mStreamingAudioImpl)
        mStreamingAudioImpl->setGain(vol);
}

// virtual
std::string LLAudioEngine::getInternetStreamURL()
{
    if (mStreamingAudioImpl)
        return mStreamingAudioImpl->getURL();

    return std::string();
}


// static
bool LLAudioEngine::acousticsEligible(const LLAudioSource* src)
{
    return src && !src->isForcedPriority() && src->getType() != AUDIO_TYPE_UI
        && !(sAcousticsKeepDry && sAcousticsKeepDry(src));
}

void LLAudioEngine::updateChannels()
{
    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        if (mChannels[i])
        {
            mChannels[i]->updateBuffer();
            mChannels[i]->update3DPosition();
            mChannels[i]->updateLoop();
        }
    }
}

void LLAudioEngine::idle()
{
    // "Update" all of our audio sources, clean up dead ones.
    // Primarily does position updating, cleanup of unused audio sources.
    // Also does regeneration of the current priority of each audio source.

    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        if (mBuffers[i])
        {
            mBuffers[i]->mInUse = false;
        }
    }

    F32 max_priority = -1.f;
    LLAudioSource *max_sourcep = NULL; // Maximum priority source without a channel
    source_map::iterator iter;
    for (iter = mAllSources.begin(); iter != mAllSources.end();)
    {
        LLAudioSource *sourcep = iter->second;

        // Update this source
        sourcep->update();
        sourcep->updatePriority();

        if (sourcep->isDone())
        {
            // The source is done playing, clean it up.
            noteSourceEnded(sourcep);   // #891: say why, if its clip never played
            delete sourcep;
            iter = mAllSources.erase(iter);
            continue;
        }

        if (sourcep->isMuted())
        {
            ++iter;
            continue;
        }

        // #891: not a clip that already played (its channel was reused after it ended):
        // giving it a channel replayed it from the start while its queued clip waited.
        // Loops do come back.
        if (!sourcep->getChannel() && sourcep->getCurrentBuffer()
            && (!sourcep->mPlayedOnce || sourcep->isLoop()))
        {
            // We could potentially play this sound if its priority is high enough.
            if (sourcep->getPriority() > max_priority)
            {
                max_priority = sourcep->getPriority();
                max_sourcep = sourcep;
            }
        }

        // Move on to the next source
        iter++;
    }

    // Now, do priority-based organization of audio sources.
    // All channels used, check priorities.
    // Find channel with lowest priority
    if (max_sourcep)
    {
        LLAudioChannel *channelp = getFreeChannel(max_priority);
        if (channelp)
        {
            //LL_INFOS() << "Replacing source in channel due to priority!" << LL_ENDL;
            max_sourcep->setChannel(channelp);
            channelp->setSource(max_sourcep);
            if (max_sourcep->isSyncSlave())
            {
                // A sync slave, it doesn't start playing until it's synced up with the master.
                // Flag this channel as waiting for sync, and return true.
                channelp->setWaiting(true);
            }
            else
            {
                channelp->setWaiting(false);
                channelp->play();
            }
        }
    }


    // Do this BEFORE we update the channels
    // Update the channels to sync up with any changes that the source made,
    // such as changing what sound was playing.
    updateChannels();

    // Update queued sounds (switch to next queued data if the current has finished playing)
    const bool walk_queued = mQueuedHint;
    bool queued_remaining = false;
    // anything queued during the walk sets the hint again via addAudioData()
    mQueuedHint = false;
    for (source_map::value_type& src_pair : mAllSources)
    {
        if (!walk_queued)
        {
            break;
        }
        // This is lame, instead of this I could actually iterate through all the sources
        // attached to each channel, since only those with active channels
        // can have anything interesting happen with their queue? (Maybe not true)
        LLAudioSource *sourcep = src_pair.second;
        if (!sourcep->mQueuedDatap)
        {
            continue;
        }
        if (sourcep->isMuted())
        {
            // Muted: stays queued, look again next frame.
            queued_remaining = true;
            continue;
        }

        // #891: advance only when the current clip is over (or can never play) AND the
        // next clip has a buffer. LL advanced unconditionally: a "no channel" source whose
        // current clip was still downloading skipped it (superseded), and a finished
        // channel was handed a bufferless next clip, alSourcePlay'd empty, flagged played
        // and the whole source (queue + preloads) deleted on the next idle.
        LLAudioChannel *channelp = sourcep->getChannel();
        bool current_over = false;
        if (!channelp)
        {
            current_over = sourcep->currentClipOver();
        }
        else if (channelp->isWaiting())
        {
            current_over = false;   // sync slave waiting for its master
        }
        else if (!channelp->isPlaying())
        {
            current_over = true;
        }
        else if (sourcep->isLoop() && channelp->mLoopedThisFrame)
        {
            // A loop finishes at the end of an iteration when something is queued.
            current_over = true;
        }

        if (current_over)
        {
            advanceQueue(sourcep);
        }

        if (sourcep->mQueuedDatap)
        {
            queued_remaining = true;
        }
    }
    if (queued_remaining)
    {
        mQueuedHint = true;
    }

    // Lame, update the channels AGAIN.
    // Update the channels to sync up with any changes that the source made,
    // such as changing what sound was playing.
    // Only the queue pass above (and sync slaves below) change channel state after the
    // first update, so skip the second one when neither ran.
    if (walk_queued || LLAudioSource::sAnySyncMaster)
    {
        updateChannels();
    }

    // Hack!  For now, just use a global sync master;
    LLAudioSource *sync_masterp = NULL;
    LLAudioChannel *master_channelp = NULL;
    F32 max_sm_priority = -1.f;
    if (LLAudioSource::sAnySyncMaster)
    {
        for (source_map::value_type& src_pair : mAllSources)
        {
            LLAudioSource *sourcep = src_pair.second;
            if (!sourcep->isMuted() && sourcep->isSyncMaster() && sourcep->getPriority() > max_sm_priority)
            {
                sync_masterp = sourcep;
                master_channelp = sync_masterp->getChannel();
                max_sm_priority = sourcep->getPriority();
            }
        }
    }

    if (master_channelp && master_channelp->mLoopedThisFrame)
    {
        // Synchronize loop slaves with their masters
        // Update queued sounds (switch to next queued data if the current has finished playing)
        for (source_map::value_type& src_pair : mAllSources)
        {
            LLAudioSource *sourcep = src_pair.second;

            if (!sourcep->isSyncSlave())
            {
                // Not a loop slave, we don't need to do anything
                continue;
            }

            LLAudioChannel *channelp = sourcep->getChannel();
            if (!channelp)
            {
                // Not playing, don't need to bother.
                continue;
            }

            if (!channelp->isPlaying())
            {
                // Now we need to check if our loop master has just looped, and
                // start playback if that's the case.
                if (sync_masterp->getChannel())
                {
                    channelp->playSynced(master_channelp);
                    channelp->setWaiting(false);
                }
            }
        }
    }

    // Sync up everything that the audio engine needs done.
    commitDeferredChanges();

    // Flush unused buffers that are stale enough
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        if (mBuffers[i])
        {
            if (!mBuffers[i]->mInUse && mBuffers[i]->mLastUseTimer.getElapsedTimeF32() > 30.f)
            {
                //LL_INFOS() << "Flushing unused buffer!" << LL_ENDL;
                mBuffers[i]->mAudioDatap->mBufferp = NULL;
                delete mBuffers[i];
                mBuffers[i] = NULL;
            }
        }
    }


    // Clear all of the looped flags for the channels
    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        if (mChannels[i])
        {
            mChannels[i]->mLoopedThisFrame = false;
        }
    }

    // Decode audio files
    LLAudioDecodeMgr::getInstance()->processQueue();

    // #891: free sound data nothing uses any more; expire bad marks.
    constexpr F32 DATA_SWEEP_SECONDS = 10.f;
    if (mDataSweepTimer.getElapsedTimeF32() >= DATA_SWEEP_SECONDS)
    {
        mDataSweepTimer.reset();
        sweepAudioData();
    }

    // #891: periodic summary of why sounds did not play (only when something changed).
    constexpr F32 ISSUE_SUMMARY_SECONDS = 300.f;
    if (mIssuesDirty && mIssueSummaryTimer.getElapsedTimeF32() >= ISSUE_SUMMARY_SECONDS)
    {
        mIssueSummaryTimer.reset();
        mIssuesDirty = false;
        std::ostringstream summary;
        for (S32 i = 0; i < ISSUE_COUNT; ++i)
        {
            summary << " " << soundIssueName((ESoundIssue)i) << "=" << mIssueCounts[i];
        }
        LL_INFOS("AudioEngine") << "Sounds not played (session totals):" << summary.str()
                                << "; sound data live=" << mAllData.size() << " peak=" << mAudioDataPeak
                                << " cap=" << AUDIO_DATA_SOFT_CAP << " bad=" << getCorruptCount()
                                << "; decode queued=" << LLAudioDecodeMgr::getInstance()->queuedCount()
                                << " in flight=" << LLAudioDecodeMgr::getInstance()->inFlightCount()
                                << "; fetches in flight=" << mTransfers.size() << LL_ENDL;
    }

    // Safety net in case we somehow missed picking it up in all the places that can
    // add or request new data. Scans every channel/source/preload map, so poll at 4 Hz
    // instead of every frame.
    constexpr F32 TRANSFER_IDLE_SCAN_SECONDS = 0.25f;
    if (mTransferIdleScanTimer.getElapsedTimeF32() >= TRANSFER_IDLE_SCAN_SECONDS)
    {
        mTransferIdleScanTimer.reset();
        startNextTransfer();
    }

    updateInternetStream();
}



bool LLAudioEngine::updateBufferForData(LLAudioData *adp, bool urgent)
{
    if (!adp)
    {
        return false;
    }

    // Update the audio buffer first - load a sound if we have it.
    // Note that this could potentially cause us to waste time updating buffers
    // for sounds that actually aren't playing, although this should be mitigated
    // by the fact that we limit the number of buffers, and we flush buffers based
    // on priority.
    if (!adp->getBuffer())
    {
        if (adp->hasDecodedData())
        {
            adp->load();
        }
        else if (adp->hasLocalData())
        {
            // #891: always by the data's own id. LL took the id as an optional argument
            // and the queue walk never passed one, so a queued clip that was in the asset
            // cache but not decoded was never decoded.
            if (!LLAudioDecodeMgr::getInstance()->addDecodeRequest(adp->getID(), urgent))
            {
                // #891: "local" was decided when the data was created; the asset cache
                // has dropped it since (or it is marked bad). Fetch it again instead of
                // waiting forever.
                adp->setHasLocalData(false);
                startNextTransfer();
                return false;
            }
        }
        else
        {
            return false;
        }
    }
    return true;
}


void LLAudioEngine::enableWind(bool enable)
{
    if (enable && (!mEnableWind))
    {
        mEnableWind = initWind();
    }
    else if (mEnableWind && (!enable))
    {
        mEnableWind = false;
        cleanupWind();
    }
}


size_t LLAudioEngine::getBufferCount() const
{
    size_t n = 0;
    for (const LLAudioBuffer* b : mBuffers)
        if (b) ++n;
    return n;
}

LLAudioBuffer * LLAudioEngine::getFreeBuffer()
{
    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        if (!mBuffers[i])
        {
            mBuffers[i] = createBuffer();
            return mBuffers[i];
        }
    }


    // Grab the oldest unused buffer
    F32 max_age = -1.f;
    S32 buffer_id = -1;
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        if (mBuffers[i])
        {
            if (!mBuffers[i]->mInUse)
            {
                if (mBuffers[i]->mLastUseTimer.getElapsedTimeF32() > max_age)
                {
                    max_age = mBuffers[i]->mLastUseTimer.getElapsedTimeF32();
                    buffer_id = i;
                }
            }
        }
    }

    if (buffer_id >= 0)
    {
        LL_DEBUGS() << "Taking over unused buffer " << buffer_id << LL_ENDL;
        //LL_INFOS() << "Flushing unused buffer!" << LL_ENDL;
        mBuffers[buffer_id]->mAudioDatap->mBufferp = NULL;
        delete mBuffers[buffer_id];
        mBuffers[buffer_id] = createBuffer();
        return mBuffers[buffer_id];
    }
    return NULL;
}


LLAudioChannel * LLAudioEngine::getFreeChannel(const F32 priority)
{
    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        if (!mChannels[i])
        {
            // No channel allocated here, use it.
            mChannels[i] = createChannel();
            return mChannels[i];
        }
        else
        {
            // Channel is allocated but not playing right now, use it.
            if (!mChannels[i]->isPlaying() && !mChannels[i]->isWaiting())
            {
                mChannels[i]->cleanup();
                if (mChannels[i]->getSource())
                {
                    mChannels[i]->getSource()->setChannel(NULL);
                }
                return mChannels[i];
            }
        }
    }

    // All channels used, check priorities.
    // Find channel with lowest priority and see if we want to replace it.
    F32 min_priority = 10000.f;
    LLAudioChannel *min_channelp = NULL;

    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        LLAudioChannel *channelp = mChannels[i];
        LLAudioSource *sourcep = channelp->getSource();
        if (sourcep->getPriority() < min_priority)
        {
            min_channelp = channelp;
            min_priority = sourcep->getPriority();
        }
    }

    if (min_priority > priority || !min_channelp)
    {
        // All playing channels have higher priority, return.
        return NULL;
    }

    // Flush the minimum priority channel, and return it.
    min_channelp->cleanup();
    min_channelp->getSource()->setChannel(NULL);
    return min_channelp;
}


void LLAudioEngine::cleanupBuffer(LLAudioBuffer *bufferp)
{
    S32 i;
    for (i = 0; i < LL_MAX_AUDIO_BUFFERS; i++)
    {
        if (mBuffers[i] == bufferp)
        {
            delete mBuffers[i];
            mBuffers[i] = NULL;
        }
    }
}


bool LLAudioEngine::preloadSound(const LLUUID &uuid)
{
    LL_DEBUGS("AudioEngine")<<"( "<<uuid<<" )"<<LL_ENDL;

    if (isCorruptSound(uuid) || isSoundBlocked(uuid))
        return false;

    getAudioData(uuid); // We don't care about the return value, this is just to make sure
                                    // that we have an entry, which will mean that the audio engine knows about this

    if (LLAudioDecodeMgr::getInstance()->addDecodeRequest(uuid, false))
    {
        // This means that we do have a local copy, and we're working on decoding it.
        return true;
    }

    return false;
}

bool LLAudioEngine::isWindEnabled()
{
    return mEnableWind;
}


void LLAudioEngine::setMuted(bool muted)
{
    if (muted != mMuted)
    {
        mMuted = muted;
        setMasterGain(mMasterGain);
    }
    enableWind(!mMuted);
}

void LLAudioEngine::setMasterGain(const F32 gain)
{
    mMasterGain = gain;
    F32 internal_gain = getMuted() ? 0.f : gain * mOutputBoost;
    if (internal_gain != mInternalGain)
    {
        mInternalGain = internal_gain;
        setInternalGain(mInternalGain);
    }
}

void LLAudioEngine::setOutputBoost(F32 boost)
{
    boost = llclamp(boost, 0.f, 8.f);
    if (boost == mOutputBoost) return;
    mOutputBoost = boost;
    setMasterGain(mMasterGain);   // (re-applies the internal gain)
}

F32 LLAudioEngine::getMasterGain()
{
    return mMasterGain;
}

void LLAudioEngine::setSecondaryGain(S32 type, F32 gain)
{
    llassert(type < LLAudioEngine::AUDIO_TYPE_COUNT);

    mSecondaryGain[type] = gain;
}

F32 LLAudioEngine::getSecondaryGain(S32 type)
{
    return mSecondaryGain[type];
}

F32 LLAudioEngine::getInternetStreamGain()
{
    if (mStreamingAudioImpl)
        return mStreamingAudioImpl->getGain();
    else
        return 1.0f;
}

void LLAudioEngine::setMaxWindGain(F32 gain)
{
    mMaxWindGain = gain;
}


F64 LLAudioEngine::mapWindVecToGain(LLVector3 wind_vec)
{
    F64 gain = 0.0;

    gain = wind_vec.magVec();

    if (gain)
    {
        if (gain > 20)
        {
            gain = 20;
        }
        gain = gain/20.0;
    }

    return (gain);
}


F64 LLAudioEngine::mapWindVecToPitch(LLVector3 wind_vec)
{
    LLVector3 listen_right;
    F64 theta;

    // Wind frame is in listener-relative coordinates
    LLVector3 norm_wind = wind_vec;
    norm_wind.normVec();
    listen_right.setVec(1.0,0.0,0.0);

    // measure angle between wind vec and listener right axis (on 0,PI)
    theta = acos(norm_wind * listen_right);

    // put it on 0, 1
    theta /= F_PI;

    // put it on [0, 0.5, 0]
    if (theta > 0.5) theta = 1.0-theta;
    if (theta < 0) theta = 0;

    return (theta);
}


F64 LLAudioEngine::mapWindVecToPan(LLVector3 wind_vec)
{
    LLVector3 listen_right;
    F64 theta;

    // Wind frame is in listener-relative coordinates
    listen_right.setVec(1.0,0.0,0.0);

    LLVector3 norm_wind = wind_vec;
    norm_wind.normVec();

    // measure angle between wind vec and listener right axis (on 0,PI)
    theta = acos(norm_wind * listen_right);

    // put it on 0, 1
    theta /= F_PI;

    return (theta);
}


LLUUID LLAudioEngine::triggerSound(const LLUUID &audio_uuid, const LLUUID& owner_id, const F32 gain,
                                 const S32 type, const LLVector3d &pos_global)
{
    // Create a new source (since this can't be associated with an existing source.
    //LL_INFOS() << "Localized: " << audio_uuid << LL_ENDL;

    if (mMuted || gain <FLT_EPSILON*2)
    {
        return LLUUID::null;
    }

    if (isSoundBlocked(audio_uuid))
    {
        return LLUUID::null;
    }

    LLUUID source_id;
    source_id.generate();

    LLAudioSource *asp = new LLAudioSource(source_id, owner_id, gain, type);
    addAudioSource(asp);
    if (pos_global.isExactlyZero())
    {
        // For sound preview and UI
        asp->setForcedPriority(true);
    }
    else
    {
        asp->setPositionGlobal(pos_global);
    }
    asp->updatePriority();
    asp->play(audio_uuid);
    return source_id;
}

void LLAudioEngine::stopSourcesPlaying(const LLUUID& audio_id)
{
    if (audio_id.isNull())
    {
        return;
    }
    for (source_map::value_type& src_pair : mAllSources)
    {
        LLAudioSource* sourcep = src_pair.second;
        if (!sourcep)
        {
            continue;
        }
        LLAudioData* cur = sourcep->getCurrentData();
        LLAudioData* queued = sourcep->getQueuedData();
        if ((cur && cur->getID() == audio_id) || (queued && queued->getID() == audio_id))
        {
            sourcep->muteForBlock();
        }
    }
}

void LLAudioEngine::triggerSound(SoundData& soundData)
{
    triggerSound(soundData.audio_uuid, soundData.owner_id, soundData.gain, soundData.type, soundData.pos_global);
}

void LLAudioEngine::setListenerPos(LLVector3 aVec)
{
    mListenerp->setPosition(aVec);
}


LLVector3 LLAudioEngine::getListenerPos()
{
    if (mListenerp)
    {
        return(mListenerp->getPosition());
    }
    else
    {
        return(LLVector3::zero);
    }
}

LLVector3 LLAudioEngine::getListenerAt()
{
    return mListenerp ? mListenerp->getAt() : LLVector3::zero;
}

LLVector3 LLAudioEngine::getListenerUp()
{
    return mListenerp ? mListenerp->getUp() : LLVector3::zero;
}


void LLAudioEngine::setListenerVelocity(LLVector3 aVec)
{
    mListenerp->setVelocity(aVec);
}


void LLAudioEngine::translateListener(LLVector3 aVec)
{
    mListenerp->translate(aVec);
}


void LLAudioEngine::orientListener(LLVector3 up, LLVector3 at)
{
    mListenerp->orient(up, at);
}


void LLAudioEngine::setListener(LLVector3 pos, LLVector3 vel, LLVector3 up, LLVector3 at)
{
    mListenerp->set(pos,vel,up,at);
}


void LLAudioEngine::setDopplerFactor(F32 factor)
{
    if (mListenerp)
    {
        mListenerp->setDopplerFactor(factor);
    }
}


F32 LLAudioEngine::getDopplerFactor()
{
    if (mListenerp)
    {
        return mListenerp->getDopplerFactor();
    }
    else
    {
        return 0.f;
    }
}


void LLAudioEngine::setRolloffFactor(F32 factor)
{
    if (mListenerp)
    {
        mListenerp->setRolloffFactor(factor);
    }
}


F32 LLAudioEngine::getRolloffFactor()
{
    if (mListenerp)
    {
        return mListenerp->getRolloffFactor();
    }
    else
    {
        return 0.f;
    }
}


void LLAudioEngine::commitDeferredChanges()
{
    mListenerp->commitDeferredChanges();
}


LLAudioSource * LLAudioEngine::findAudioSource(const LLUUID &source_id)
{
    source_map::iterator iter;
    iter = mAllSources.find(source_id);

    if (iter == mAllSources.end())
    {
        return NULL;
    }
    else
    {
        return iter->second;
    }
}


LLAudioData * LLAudioEngine::getAudioData(const LLUUID &audio_uuid)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_MEDIA;
    if(isCorruptSound(audio_uuid))
        return nullptr;

    auto iter = mAllData.find(audio_uuid);
    if (iter == mAllData.end())
    {
        // Create the new audio data
        LLAudioData *adp = new LLAudioData(audio_uuid);
        adp->mLastTouch = LLFrameTimer::getElapsedSeconds();
        mAllData[audio_uuid] = adp;
        mAudioDataPeak = llmax(mAudioDataPeak, mAllData.size());
        return adp;
    }
    else
    {
        iter->second->mLastTouch = LLFrameTimer::getElapsedSeconds();
        return iter->second;
    }
}

LLAudioData * LLAudioEngine::findAudioData(const LLUUID &audio_uuid) const
{
    auto iter = mAllData.find(audio_uuid);
    return iter == mAllData.end() ? nullptr : iter->second;
}

void LLAudioEngine::addAudioSource(LLAudioSource *asp)
{
    mAllSources[asp->getID()] = asp;
}


void LLAudioEngine::cleanupAudioSource(LLAudioSource *asp)
{
    source_map::iterator iter;
    iter = mAllSources.find(asp->getID());
    if (iter == mAllSources.end())
    {
        LL_WARNS() << "Cleaning up unknown audio source!" << LL_ENDL;
        return;
    }
    else
    {
        LL_DEBUGS("AudioEngine") << "Cleaning up audio sources for "<< asp->getID() <<LL_ENDL;
        delete asp;
        mAllSources.erase(iter);
    }
}

bool LLAudioEngine::hasDecodedFile(const LLUUID &uuid)
{
    // The sound cache index, not a stat per call (#176). It only ever lists files that
    // were renamed into place complete.
    return LLSoundCache::has(uuid);
}

void LLAudioEngine::handleDecodedFileEvicted(const LLUUID &uuid)
{
    data_map::iterator iter = mAllData.find(uuid);
    if (iter == mAllData.end() || !iter->second)
    {
        return;
    }
    LLAudioData *adp = iter->second;
    if (adp->getBuffer())
    {
        // Already loaded: loadWAV() read the whole file into the buffer, so playback is
        // unaffected. If the buffer is recycled later, load() finds the file gone and
        // re-decodes then.
        return;
    }
    // Not loaded: forget the decoded file so the next use decodes (or fetches) again.
    // No preload here -- re-decoding every evicted sound at once would just evict others.
    adp->setHasDecodedData(false);
    adp->setHasCompletedDecode(false);
    adp->setHasWAVLoadFailed(false);
    adp->setHasLocalData(gAssetStorage && gAssetStorage->hasLocalAsset(uuid, LLAssetType::AT_SOUND));
}


bool LLAudioEngine::hasLocalFile(const LLUUID &uuid)
{
    // See if it's in the cache.
    bool have_local = LLFileSystem::getExists(uuid, LLAssetType::AT_SOUND);
    LL_DEBUGS("AudioEngine") << "sound uuid " << uuid << " exists in cache" << LL_ENDL;
    return have_local;
}

bool LLAudioEngine::isTransferInFlight(const LLUUID& id) const
{
    for (const Transfer& t : mTransfers)
    {
        if (t.id == id)
        {
            return true;
        }
    }
    return false;
}

LLUUID LLAudioEngine::pickNextTransferCandidate()
{
    // Get the ID for the next asset that we want to transfer.
    // Pick one in the following order:
    LLUUID asset_id;
    S32 i;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    LLAudioSource *asp = NULL;
    LLAudioData *adp = NULL;
    data_map::iterator data_iter;

    // Check all channels for currently playing sounds.
    F32 max_pri = -1.f;
    for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
    {
        if (!mChannels[i])
        {
            continue;
        }

        asp = mChannels[i]->getSource();
        if (!asp)
        {
            continue;
        }
        if (asp->getPriority() <= max_pri)
        {
            continue;
        }

        if (asp->getPriority() <= max_pri)
        {
            continue;
        }

        adp = asp->getCurrentData();
        if (!adp)
        {
            continue;
        }

        if (wantsFetch(adp, now))
        {
            asset_id = adp->getID();
            max_pri = asp->getPriority();
        }
    }

    // Check all channels for currently queued sounds.
    if (asset_id.isNull())
    {
        max_pri = -1.f;
        for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
        {
            if (!mChannels[i])
            {
                continue;
            }

            LLAudioSource *asp;
            asp = mChannels[i]->getSource();
            if (!asp)
            {
                continue;
            }

            if (asp->getPriority() <= max_pri)
            {
                continue;
            }

            adp = asp->getQueuedData();
            if (!adp)
            {
                continue;
            }

            if (wantsFetch(adp, now))
            {
                asset_id = adp->getID();
                max_pri = asp->getPriority();
            }
        }
    }

    // Check all live channels for other sounds (preloads).
    if (asset_id.isNull())
    {
        max_pri = -1.f;
        for (i = 0; i < LL_MAX_AUDIO_CHANNELS; i++)
        {
            if (!mChannels[i])
            {
                continue;
            }

            LLAudioSource *asp;
            asp = mChannels[i]->getSource();
            if (!asp)
            {
                continue;
            }

            if (asp->getPriority() <= max_pri)
            {
                continue;
            }


            for (data_map::value_type& preload_pair : asp->mPreloadMap)
            {
                LLAudioData *adp = preload_pair.second;
                if (!adp)
                {
                    continue;
                }

                if (wantsFetch(adp, now))
                {
                    asset_id = adp->getID();
                    max_pri = asp->getPriority();
                }
            }
        }
    }

    // Check all sources
    if (asset_id.isNull())
    {
        max_pri = -1.f;
        source_map::iterator source_iter;
        for (source_map::value_type& source_pair : mAllSources)
        {
            asp = source_pair.second;
            if (!asp)
            {
                continue;
            }

            if (asp->getPriority() <= max_pri)
            {
                continue;
            }

            adp = asp->getCurrentData();
            if (wantsFetch(adp, now))
            {
                asset_id = adp->getID();
                max_pri = asp->getPriority();
                continue;
            }

            adp = asp->getQueuedData();
            if (wantsFetch(adp, now))
            {
                asset_id = adp->getID();
                max_pri = asp->getPriority();
                continue;
            }

            for (data_map::value_type& preload_pair : asp->mPreloadMap)
            {
                LLAudioData *adp = preload_pair.second;
                if (!adp)
                {
                    continue;
                }

                if (wantsFetch(adp, now))
                {
                    asset_id = adp->getID();
                    max_pri = asp->getPriority();
                    break;
                }
            }
        }
    }

    return asset_id;
}

void LLAudioEngine::startNextTransfer()
{
    // A transfer that never calls back would otherwise hold its slot until the asset
    // system's own timeout. Give up waiting on it after a while; its late callback is
    // still handled.
    // #891: 30 s (was 60) so a few stuck fetches cannot hold every slot for a minute; the
    // timeout counts as a failed attempt and is retried with backoff (it was a
    // session-long "decode failed" before, so the sound was never fetched again).
    constexpr F32 TRANSFER_TIMEOUT_SECONDS = 30.f;
    for (size_t i = 0; i < mTransfers.size();)
    {
        if (mTransfers[i].timer.getElapsedTimeF32() > TRANSFER_TIMEOUT_SECONDS)
        {
            const LLUUID timed_out = mTransfers[i].id;
            mTransfers.erase(mTransfers.begin() + i);
            LLAudioData *adp = findAudioData(timed_out);
            if (adp && !adp->hasLocalData())
            {
                // A slow asset service is not a bad asset: a timeout only backs off (it
                // does not count toward the bad mark; error callbacks do). A late success
                // is still handled by assetCallback.
                adp->mRetryAt = (F64)LLFrameTimer::getElapsedSeconds() + 5.0;
                noteSoundIssue(adp, timed_out, ISSUE_FETCH_FAILED,
                               llformat("no answer in %.0f s; asking again", TRANSFER_TIMEOUT_SECONDS));
            }
        }
        else
        {
            ++i;
        }
    }

    // getAssetData() can complete synchronously and re-enter here via assetCallback(),
    // so hold no iterators across it and re-check the bound every pass.
    while (mTransfers.size() < MAX_CONCURRENT_TRANSFERS && !getMuted())
    {
        const LLUUID asset_id = pickNextTransferCandidate();
        if (asset_id.isNull())
        {
            break;
        }
        LL_DEBUGS("AudioEngine") << "Getting asset data for: " << asset_id << LL_ENDL;
        mTransfers.emplace_back();
        mTransfers.back().id = asset_id;
        gAssetStorage->getAssetData(asset_id, LLAssetType::AT_SOUND,
                                    assetCallback, NULL);
    }
}


// static
void LLAudioEngine::assetCallback(const LLUUID &uuid, LLAssetType::EType type, void *user_data, S32 result_code, LLExtStat ext_status)
{
    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return;
    }

    // Was this still one of ours? A transfer that timed out was already counted as a
    // failed attempt; a late failure must not count twice.
    bool tracked = false;
    for (size_t i = 0; i < gAudiop->mTransfers.size(); ++i)
    {
        if (gAudiop->mTransfers[i].id == uuid)
        {
            gAudiop->mTransfers.erase(gAudiop->mTransfers.begin() + i);
            tracked = true;
            break;
        }
    }

    if (result_code)
    {
        // #891: LL marked the sound decode-failed for the session on any error (a 503 or
        // a dropped connection included), so it was never fetched again. Now: bounded
        // retries with backoff, then a bad mark that expires.
        if (tracked)
        {
            const bool not_retryable = result_code == LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE;
            gAudiop->noteFetchFailed(uuid,
                                     llformat("%s (%d)", LLAssetStorage::getErrorString(result_code), result_code),
                                     not_retryable);
        }
    }
    else
    {
        LLAudioData *adp = gAudiop->getAudioData(uuid);
        if (!adp)
        {
            // Marked bad meanwhile (or never wanted): nothing to do.
            LL_DEBUGS("AudioEngine") << "Got asset callback without audio data for " << uuid << LL_ENDL;
        }
        else
        {
            // LL_INFOS() << "Got asset callback with good audio data for " << uuid << ", making decode request" << LL_ENDL;
            adp->setHasDecodeFailed(false);
            if (!adp->hasDecodedData())
            {
                adp->setHasCompletedDecode(false);
            }
            adp->setHasLocalData(true);
            adp->mRetryAt = 0.0;
            // #891: first in line when a source wants it now (current or queued clip).
            LLAudioDecodeMgr::getInstance()->addDecodeRequest(uuid, gAudiop->isWantedNow(uuid));
        }
    }
    gAudiop->startNextTransfer();
}


//
// LLAudioSource implementation
//


LLAudioSource::LLAudioSource(const LLUUID& id, const LLUUID& owner_id, const F32 gain, const S32 type)
:   mID(id),
    mOwnerID(owner_id),
    mPriority(0.f),
    mGain(gain),
    mSourceMuted(false),
    mForcedPriority(false),
    mLoop(false),
    mSyncMaster(false),
    mSyncSlave(false),
    mQueueSounds(false),
    mPlayedOnce(false),
    mCorrupted(false),
    mType(type),
    mChannelp(NULL),
    mCurrentDatap(NULL),
    mQueuedDatap(NULL)
{
}


LLAudioSource::~LLAudioSource()
{
    if (mChannelp)
    {
        // Stop playback of this sound
        mChannelp->setSource(NULL);
        mChannelp = NULL;
    }
}


void LLAudioSource::setChannel(LLAudioChannel *channelp)
{
    if (channelp == mChannelp)
    {
        return;
    }

    mChannelp = channelp;
}


void LLAudioSource::update()
{
    if(mCorrupted)
    {
        return ; //no need to update
    }

    prunePreloads();

    LLAudioData *adp = getCurrentData();
    if (!adp || adp->getBuffer())
    {
        return;
    }

    // (LL's "if (adp->getBuffer()) play()" here sat inside "no current buffer" -- the
    // same pointer -- so it never ran.)
    if (adp->hasDecodedData() && !adp->hasWAVLoadFailed())
    {
        // Decoded since we asked: load it. idle() hands a channel to a source with a
        // buffer and no channel; if we already hold one, start it here.
        adp->load();
        if (adp->getBuffer() && mChannelp && !mPlayedOnce && !mChannelp->isPlaying())
        {
            mChannelp->updateBuffer();
            if (!isSyncSlave())
            {
                mChannelp->play();
            }
        }
        return;
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (!adp->isUnplayable(now))
    {
        // #891: still on its way. Once a second, make sure something is moving it: a
        // decode for a cached asset (its decoded file may have been evicted, or a bad mark
        // just expired), else the fetch (the asset cache may have dropped it).
        constexpr F32 NUDGE_SECONDS = 1.f;
        if (gAudiop && mDecodeNudgeTimer.getElapsedTimeF32() >= NUDGE_SECONDS)
        {
            mDecodeNudgeTimer.reset();
            if (adp->hasLocalData() && !adp->hasDecodedData()
                && !LLAudioDecodeMgr::getInstance()->isPending(adp->getID()))
            {
                gAudiop->updateBufferForData(adp, true);
            }
        }
        return;
    }

    {
        // Out of retries (the fetch / decode failure already said why, once).
        if (mQueuedDatap)
        {
            // #891 isolation: LL marked the whole source corrupt, which deleted it with
            // its queued clip and preloads. Treat the bad clip as over; the queue walk
            // starts the next one (currentClipOver() is true once it "played").
            mPlayedOnce = true;
            if (gAudiop)
            {
                gAudiop->noteSourceQueued();
            }
        }
        else
        {
            LL_DEBUGS("AudioEngine") << "LLAudioSource " << mID << " done: " << adp->getID() << " cannot be played" << LL_ENDL;
            mCorrupted = true;
        }
    }
}

void LLAudioSource::setCurrentData(LLAudioData* adp)
{
    // Every caller is starting a clip, the same one again included ("play A, queue A").
    mCurrentDatap = adp;
    mPlayedOnce = false;
    mCurrentTimer.reset();
    mAgeTimer.reset();
}

void LLAudioSource::prunePreloads()
{
    if (mPreloadMap.empty())
    {
        return;
    }
    constexpr F32 PRELOAD_CHECK_SECONDS = 0.5f;
    if (mPreloadCheckTimer.getElapsedTimeF32() < PRELOAD_CHECK_SECONDS)
    {
        return;
    }
    mPreloadCheckTimer.reset();

    const F64 now = LLFrameTimer::getElapsedSeconds();
    for (data_map::iterator it = mPreloadMap.begin(); it != mPreloadMap.end();)
    {
        LLAudioData* adp = it->second;
        if (!adp || adp->hasDecodedData() || adp->getBuffer() || adp->isUnplayable(now))
        {
            // #891: done (decoded file on disk) or never going to be: a preload's job is
            // over. LL kept every preload for the source's life (unbounded per object,
            // and an entry whose decoded file was evicted stayed "pending" forever).
            it = mPreloadMap.erase(it);
            continue;
        }
        if (adp->hasLocalData() && !adp->hasCompletedDecode()
            && !LLAudioDecodeMgr::getInstance()->addDecodeRequest(adp->getID(), false))
        {
            // #891: preloads already in the asset cache were never decoded (only a fetch
            // callback queued a decode). Decode only (no buffer load); if the asset cache
            // dropped it, let the transfer picker fetch it.
            adp->setHasLocalData(false);
        }
        ++it;
    }
}

bool LLAudioSource::currentClipOver() const
{
    if (!mCurrentDatap || mPlayedOnce)
    {
        return true;    // nothing current, or it played (and finished / lost its channel)
    }
    if (mCurrentDatap->isUnplayable(LLFrameTimer::getElapsedSeconds()))
    {
        return true;
    }
    // Never started: give it the unplayed age to load and get a channel, then move on
    // so one slow clip does not hold up the rest of the queue.
    constexpr F32 MAX_WAIT_TO_START = 15.f;
    return mCurrentTimer.getElapsedTimeF32() > MAX_WAIT_TO_START;
}

void LLAudioSource::restartCurrent()
{
    if (!mCurrentDatap || !gAudiop || isMuted())
    {
        return;
    }
    if (LLAudioEngine::isSoundBlocked(mCurrentDatap->getID()))
    {
        return;
    }
    mAgeTimer.reset();
    mCurrentTimer.reset();
    mPlayedOnce = false;
    if (!gAudiop->updateBufferForData(mCurrentDatap, true) || !mCurrentDatap->getBuffer())
    {
        return;     // update() / idle() start it once it is loaded
    }
    if (!setupChannel())
    {
        return;
    }
    if (isSyncSlave())
    {
        getChannel()->setWaiting(true);
        return;
    }
    getChannel()->play();
}

void LLAudioSource::updatePriority()
{
    if (isForcedPriority())
    {
        mPriority = 1.f;
    }
    else if (isMuted())
    {
        mPriority = 0.f;
    }
    else
    {
        // Priority is based on distance
        LLVector3 dist_vec;
        dist_vec.setVec(getPositionGlobal());

        if (gAudiop)
        {
            dist_vec -= gAudiop->getListenerPos();
        }

        F32 dist_squared = llmax(1.f, dist_vec.magVecSquared());

        mPriority = mGain / dist_squared;
    }
}

bool LLAudioSource::setupChannel()
{
    LLAudioData *adp = getCurrentData();

    if (!adp || !adp->getBuffer())
    {
        // We're not ready to play back the sound yet, so don't try and allocate a channel for it.
        //LL_WARNS() << "Aborting, no buffer" << LL_ENDL;
        return false;
    }

    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return false;
    }

    if (!mChannelp)
    {
        // Update the priority, in case we need to push out another channel.
        updatePriority();

        setChannel(gAudiop->getFreeChannel(getPriority()));
    }

    if (!mChannelp)
    {
        // Ugh, we don't have any free channels.
        // Now we have to reprioritize.
        // For now, just don't play the sound.
        //LL_WARNS() << "Aborting, no free channels" << LL_ENDL;
        return false;
    }

    mChannelp->setSource(this);
    return true;
}

void LLAudioSource::stop()
{
    if (mCurrentDatap && !mPlayedOnce && !isMuted() && gAudiop)
    {
        // #891: replaced (non-queued play) or stopped before it ever started.
        gAudiop->noteSoundIssue(mCurrentDatap, mCurrentDatap->getID(), LLAudioEngine::ISSUE_SUPERSEDED,
                                mCurrentDatap->getBuffer() ? "it was ready" : "it was still loading");
    }
    play(LLUUID::null);
    if (mCurrentDatap)
    {
        // always reset data if something wants us to stop
        mCurrentDatap = nullptr;
    }
}

bool LLAudioSource::isChannelPlaying() const
{
    return mChannelp && mChannelp->isPlaying();
}

void LLAudioSource::muteForBlock()
{
    mSourceMuted = true;
    mQueuedDatap = nullptr;
    if (getChannel())
    {
        getChannel()->setSource(NULL);
        setChannel(NULL);
    }
}

bool LLAudioSource::play(const LLUUID &audio_uuid)
{
    // Special abuse of play(); don't play a sound, but kill it.
    if (audio_uuid.isNull())
    {
        if (getChannel())
        {
            getChannel()->setSource(NULL);
            setChannel(NULL);
            if (!isMuted())
            {
                mCurrentDatap = NULL;
            }
        }
        return false;
    }

    // #654: never fetch/decode/play a blocked asset
    if (LLAudioEngine::isSoundBlocked(audio_uuid))
    {
        // Remember which asset this source wanted (no fetch/queue: the transfer scan skips
        // blocked ids) so sources that recompute mute state per frame can restart it live
        // when the block is removed.
        if (gAudiop && !mCurrentDatap)
        {
            mCurrentDatap = gAudiop->getAudioData(audio_uuid);
        }
        return false;
    }

    // Reset our age timeout if someone attempts to play the source.
    mAgeTimer.reset();

    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return false;
    }

    LLAudioData *adp = gAudiop->getAudioData(audio_uuid);
    addAudioData(adp);

    if (isMuted())
    {
        return false;
    }

    bool has_buffer = gAudiop->updateBufferForData(adp, true);
    if (!has_buffer)
    {
        // Don't bother trying to set up a channel or anything, we don't have an audio buffer.
        return false;
    }

    if (!setupChannel())
    {
        return false;
    }

    if (isSyncSlave())
    {
        // A sync slave, it doesn't start playing until it's synced up with the master.
        // Flag this channel as waiting for sync, and return true.
        getChannel()->setWaiting(true);
        return true;
    }

    // #891: never restart a clip that already played. With queueing on, a play() that
    // arrived after the current clip ended (source kept alive by pending preloads)
    // replayed the old clip from the start, then the new one: the player fell a whole
    // clip behind and later clips hit a full queue. The queue walk starts the next clip.
    if (!mPlayedOnce)
    {
        getChannel()->play();
    }
    return true;
}


bool LLAudioSource::isDone() const
{
    if(mCorrupted)
    {
        return true ;
    }

    const F32 MAX_AGE = 60.f;
    const F32 MAX_UNPLAYED_AGE = 15.f;
    const F32 MAX_MUTED_AGE = 11.f;
    // #891: a sound still being fetched / decoded / retried gets longer before the source
    // gives up on it (a cold fetch plus one retry backoff can exceed 15 s).
    const F32 MAX_LOADING_AGE = 30.f;

    if (isLoop())
    {
        // Looped sources never die on their own.
        return false;
    }

    if (hasPendingPreloads())
    {
        return false;
    }

    if (mQueuedDatap)
    {
        // Don't kill this sound if we've got something queued up to play.
        return false;
    }

    F32 elapsed = mAgeTimer.getElapsedTimeF32();

    // This is a single-play source
    if (!mChannelp)
    {
        F32 max_age = mSourceMuted ? MAX_MUTED_AGE : MAX_UNPLAYED_AGE;
        if (!mSourceMuted && mCurrentDatap && mCurrentDatap->isLoadPending(LLFrameTimer::getElapsedSeconds()))
        {
            max_age = MAX_LOADING_AGE;
        }
        if ((elapsed > max_age) || mPlayedOnce)
        {
            // We don't have a channel assigned, and it's been
            // over 15 seconds since we tried to play it.  Don't bother.
            //LL_INFOS() << "No channel assigned, source is done" << LL_ENDL;
            return true;
        }
        else
        {
            return false;
        }
    }

    if (mChannelp->isPlaying())
    {
        if (elapsed > MAX_AGE)
        {
            // Arbitarily cut off non-looped sounds when they're old.
            return true;
        }
        else
        {
            // Sound is still playing and we haven't timed out, don't kill it.
            return false;
        }
    }

    if ((elapsed > MAX_UNPLAYED_AGE) || mPlayedOnce)
    {
        // The sound isn't playing back after 15 seconds or we're already done playing it, kill it.
        return true;
    }

    return false;
}


void LLAudioSource::addAudioData(LLAudioData *adp, const bool set_current)
{
    // Only handle a single piece of audio data associated with a source right now,
    // until I implement prefetch.

    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return;
    }

    // #654: blocked assets are not queued, preloaded or fetched
    if (adp && LLAudioEngine::isSoundBlocked(adp->getID()))
    {
        return;
    }

    if (set_current)
    {
        if (!mCurrentDatap)
        {
            setCurrentData(adp);
            if (mChannelp)
            {
                mChannelp->updateBuffer();
                mChannelp->play();
            }

            // Make sure the audio engine knows that we want to request this sound.
            gAudiop->startNextTransfer();
            return;
        }
        else if (mQueueSounds)
        {
            // If we have current data, and we're queuing, put
            // the object onto the queue.
            if (mQueuedDatap)
            {
                // We only queue one sound at a time, and it's a FIFO.
                // Don't put it onto the queue. (SL semantics: one clip playing, one
                // queued; later ones are dropped.)
                if (adp && adp != mQueuedDatap)
                {
                    gAudiop->noteSoundIssue(adp, adp->getID(), LLAudioEngine::ISSUE_QUEUE_FULL,
                                            llformat("source %s already has %s queued", mID.asString().c_str(),
                                                     mQueuedDatap->getID().asString().c_str()));
                }
                return;
            }

            if (adp == mCurrentDatap && isLoop())
            {
                // No point in queueing the same sound if
                // we're looping.
                return;
            }
            mQueuedDatap = adp;
            gAudiop->noteSourceQueued();

            // Make sure the audio engine knows that we want to request this sound.
            gAudiop->startNextTransfer();
        }
        else
        {
            if (mCurrentDatap != adp)
            {
                // Right now, if we're currently playing this sound in a channel, we
                // update the buffer that the channel's associated with
                // and play it.  This may not be the correct behavior.
                if (!mPlayedOnce && !isMuted())
                {
                    gAudiop->noteSoundIssue(mCurrentDatap, mCurrentDatap->getID(), LLAudioEngine::ISSUE_SUPERSEDED,
                                            "replaced by another sound before it started");
                }
                setCurrentData(adp);
                if (mChannelp)
                {
                    mChannelp->updateBuffer();
                    mChannelp->play();
                }
                // Make sure the audio engine knows that we want to request this sound.
                gAudiop->startNextTransfer();
            }
        }
    }
    else if (adp)
    {
        // Add it to the preload list.
        mPreloadMap[adp->getID()] = adp;
        // #891: decode now if it is already in the asset cache (LL only fetched, and a
        // fetch is skipped for local assets, so cached preloads were never decoded).
        // Decode only: loading the buffer here would read every preloaded clip into an
        // AL buffer on the main thread.
        if (adp->hasLocalData() && !adp->hasDecodedData() && !adp->hasCompletedDecode()
            && !LLAudioDecodeMgr::getInstance()->addDecodeRequest(adp->getID(), false))
        {
            adp->setHasLocalData(false);    // the asset cache dropped it: fetch it
        }
        gAudiop->startNextTransfer();
    }
}


bool LLAudioSource::hasPendingPreloads() const
{
    // Check to see if we've got any preloads on deck for this source
    for (const data_map::value_type& preload_pair : mPreloadMap)
    {
        LLAudioData *adp = preload_pair.second;
        // note: a bad UUID will forever be !hasDecodedData()
        // but also hasDecodeFailed(), hence the check for hasDecodeFailed()
        if (!adp)
        {
            continue;
        }
        if (!adp->hasDecodedData() && !adp->hasDecodeFailed())
        {
            // This source is still waiting for a preload
            return true;
        }
    }

    return false;
}


LLAudioData * LLAudioSource::getCurrentData()
{
    return mCurrentDatap;
}

LLAudioData * LLAudioSource::getQueuedData()
{
    return mQueuedDatap;
}

LLAudioBuffer * LLAudioSource::getCurrentBuffer()
{
    if (!mCurrentDatap)
    {
        return NULL;
    }

    return mCurrentDatap->getBuffer();
}




//
// LLAudioChannel implementation
//


LLAudioChannel::LLAudioChannel() :
    mCurrentSourcep(NULL),
    mCurrentBufferp(NULL),
    mLoopedThisFrame(false),
    mWaiting(false),
    mSecondaryGain(1.0f)
{
}


LLAudioChannel::~LLAudioChannel()
{
    // Need to disconnect any sources which are using this channel.
    //LL_INFOS() << "Cleaning up audio channel" << LL_ENDL;
    if (mCurrentSourcep)
    {
        mCurrentSourcep->setChannel(NULL);
    }
    mCurrentBufferp = NULL;
}


void LLAudioChannel::setSource(LLAudioSource *sourcep)
{
    //LL_INFOS() << this << ": setSource(" << sourcep << ")" << LL_ENDL;

    if (!sourcep)
    {
        // Clearing the source for this channel, don't need to do anything.
        //LL_INFOS() << "Clearing source for channel" << LL_ENDL;
        cleanup();
        mCurrentSourcep = NULL;
        mWaiting = false;
    }
    else
    {
        LL_DEBUGS("AudioEngine") << "( id: " << sourcep->getID() << ")" << LL_ENDL;

        if (sourcep == mCurrentSourcep)
        {
            // Don't reallocate the channel, this will make FMOD goofy.
            //LL_INFOS() << "Calling setSource with same source!" << LL_ENDL;
        }
        else
        {
            // #683: another source's occlusion filter must not carry over (the acoustics
            // driver sets this source's own on its next update).
            mAcGain = 1.f;
            mAcGainHF = 1.f;
            mAcSendGain = 1.f;
            mAcSendGainHF = 1.f;
        }

        mCurrentSourcep = sourcep;

        updateBuffer();
        update3DPosition();
    }
}

bool LLAudioChannel::updateBuffer()
{
    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return false;
    }

    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing
        // to be updated
        return false;
    }

    // Initialize the channel's gain setting for this sound.
    setSecondaryGain(gAudiop->getSecondaryGain(mCurrentSourcep->getType()));

    LLAudioBuffer *bufferp = mCurrentSourcep->getCurrentBuffer();
    if (bufferp == mCurrentBufferp)
    {
        if (bufferp)
        {
            // The source hasn't changed what buffer it's playing
            bufferp->mLastUseTimer.reset();
            bufferp->mInUse = true;
        }
        return false;
    }

    //
    // The source changed what buffer it's playing.  We need to clean up
    // the existing channel
    //
    cleanup();

    mCurrentBufferp = bufferp;
    if (bufferp)
    {
        bufferp->mLastUseTimer.reset();
        bufferp->mInUse = true;
    }

    if (!mCurrentBufferp)
    {
        // There's no new buffer to be played, so we just abort.
        return false;
    }

    return true;
}




//
// LLAudioData implementation
//


LLAudioData::LLAudioData(const LLUUID &uuid) :
    mID(uuid),
    mBufferp(NULL),
    mHasLocalData(false),
    mHasDecodedData(false),
    mHasCompletedDecode(false),
    mHasDecodeFailed(false),
    mHasWAVLoadFailed(false),
    mRedecodeAttempted(false)
{
    if (uuid.isNull())
    {
        // This is a null sound.
        return;
    }

    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        return;
    }

    if (gAudiop->hasDecodedFile(uuid))
    {
        // Already have a decoded version, don't need to decode it.
        setHasLocalData(true);
        setHasDecodedData(true);
        setHasCompletedDecode(true);
    }
    else if (gAssetStorage && gAssetStorage->hasLocalAsset(uuid, LLAssetType::AT_SOUND))
    {
        setHasLocalData(true);
    }
}

//return false when the audio file is corrupted.
bool LLAudioData::load()
{
    // For now, just assume we're going to use one buffer per audiodata.
    if (mBufferp)
    {
        // We already have this sound in a buffer, don't do anything.
        LL_INFOS() << "Already have a buffer for this sound, don't bother loading!" << LL_ENDL;
        mHasWAVLoadFailed = false;
        return true;
    }

    if (!gAudiop)
    {
        LL_WARNS("AudioEngine") << "LLAudioEngine instance doesn't exist!" << LL_ENDL;
        mHasWAVLoadFailed = true;
        return false;
    }

    mBufferp = gAudiop->getFreeBuffer();
    if (!mBufferp)
    {
        // No free buffers, abort. #891: not a WAV failure -- LL set mHasWAVLoadFailed
        // here, which stopped update() from ever loading this sound again. Retried on the
        // next update.
        LL_DEBUGS("AudioEngine") << "Not able to allocate a new audio buffer, retrying later." << LL_ENDL;
        return true;
    }

    const std::string wav_path = LLSoundCache::getPath(mID);

    // loadWAV() reads the whole file into the AL buffer, so once it returns the file
    // is no longer needed and the sound cache may evict it at any time.
    mHasWAVLoadFailed = !mBufferp->loadWAV(wav_path);
    if (mHasWAVLoadFailed)
    {
        // Hrm.  Right now, let's unset the buffer, since it's empty.
        gAudiop->cleanupBuffer(mBufferp);
        mBufferp = nullptr;

        bool redecode = true;
        if (gDirUtilp->fileExists(wav_path))
        {
            // Present but unreadable (truncated / corrupt, e.g. written by an older build
            // that did not write via a temp file): never trust it again. Decode afresh
            // once; if that output fails too, give up on the sound for this session.
            LL_WARNS("AudioEngine") << "Decoded sound " << wav_path << " is unreadable; removing it" << LL_ENDL;
            redecode = !mRedecodeAttempted;
            mRedecodeAttempted = true;
        }
        // Drop the file (if any) and its index entry either way.
        LLSoundCache::remove(mID);

        if (redecode)
        {
            mHasLocalData = false;
            mHasDecodedData = false;
            mHasCompletedDecode = false;
            mHasDecodeFailed = false;
            mHasWAVLoadFailed = false;
            gAudiop->preloadSound(mID);
        }
        else
        {
            // Quiet for the rest of the session: no local data means updateBufferForData
            // will not queue another decode, and decode-failed keeps it off the fetch list.
            mHasLocalData = false;
            mHasDecodedData = false;
            gAudiop->noteDecodeFailed(mID, "decoded file unreadable twice");
        }

        return false;
    }
    LLSoundCache::touch(mID);
    mBufferp->mAudioDatap = this;
    return true;
}

void LLAudioEngine::markSoundCorrupt(const LLUUID& sound_id)
{
    if (LLAudioData* adp = findAudioData(sound_id))
    {
        markBad(adp, "marked corrupt");
    }
}

bool LLAudioEngine::isCorruptSound(const LLUUID& sound_id)
{
    if (sound_id.isNull()) return true;

    LLAudioData* adp = findAudioData(sound_id);
    if (!adp || adp->mBadUntil <= 0.0)
    {
        return false;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (expireBadMark(adp, now))
    {
        return false;
    }
    return adp->isBad(now);
}

size_t LLAudioEngine::getCorruptCount() const
{
    const F64 now = LLFrameTimer::getElapsedSeconds();
    size_t n = 0;
    for (const data_map::value_type& d : mAllData)
    {
        if (d.second && d.second->isBad(now))
        {
            ++n;
        }
    }
    return n;
}

//
// #891 sound diagnostics, retries and bounded data
//

// static
const char* LLAudioEngine::soundIssueName(ESoundIssue issue)
{
    switch (issue)
    {
    case ISSUE_FETCH_FAILED:  return "fetch_failed";
    case ISSUE_DECODE_FAILED: return "decode_failed";
    case ISSUE_MARKED_BAD:    return "marked_bad";
    case ISSUE_NO_CHANNEL:    return "no_channel";
    case ISSUE_NOT_READY:     return "not_ready";
    case ISSUE_MUTED:         return "muted_or_out_of_range";
    case ISSUE_SUPERSEDED:    return "superseded";
    case ISSUE_QUEUE_FULL:    return "queue_full";
    case ISSUE_QUEUED_BAD:    return "queued_unplayable";
    default:                  return "unknown";
    }
}

void LLAudioEngine::noteSoundIssue(LLAudioData* adp, const LLUUID& id, ESoundIssue issue, const std::string& detail)
{
    if (issue >= ISSUE_COUNT)
    {
        return;
    }
    ++mIssueCounts[issue];
    mIssuesDirty = true;
    if (!adp)
    {
        return;     // counted; nothing per-sound to remember "logged once" on
    }
    const U16 bit = (U16)(1u << issue);
    if (adp->mReported & bit)
    {
        LL_DEBUGS("AudioEngine") << "Sound " << id << " not played: " << soundIssueName(issue)
                                 << (detail.empty() ? "" : " (" + detail + ")") << LL_ENDL;
        return;
    }
    adp->mReported |= bit;
    const bool serious = issue == ISSUE_FETCH_FAILED || issue == ISSUE_DECODE_FAILED || issue == ISSUE_MARKED_BAD;
    if (serious)
    {
        LL_WARNS("AudioEngine") << "Sound " << id << " not played: " << soundIssueName(issue)
                                << (detail.empty() ? "" : " (" + detail + ")") << LL_ENDL;
    }
    else
    {
        LL_INFOS("AudioEngine") << "Sound " << id << " not played: " << soundIssueName(issue)
                                << (detail.empty() ? "" : " (" + detail + ")") << LL_ENDL;
    }
}

bool LLAudioEngine::isWantedNow(const LLUUID& id) const
{
    for (const source_map::value_type& sp : mAllSources)
    {
        const LLAudioSource* asp = sp.second;
        if (asp && ((asp->mCurrentDatap && asp->mCurrentDatap->getID() == id)
                    || (asp->mQueuedDatap && asp->mQueuedDatap->getID() == id)))
        {
            return true;
        }
    }
    return false;
}

bool LLAudioEngine::wantsFetch(const LLAudioData* adp, F64 now) const
{
    return adp
        && adp->getID().notNull()
        && !adp->hasLocalData()
        && !adp->hasDecodeFailed()
        && !adp->isBad(now)
        && now >= adp->mRetryAt
        && !isTransferInFlight(adp->getID())
        && !isSoundBlocked(adp->getID());
}

void LLAudioEngine::markBad(LLAudioData* adp, const std::string& why)
{
    if (!adp)
    {
        return;
    }
    adp->mBadUntil = (F64)LLFrameTimer::getElapsedSeconds() + BAD_SOUND_TTL;
    adp->mRetryAt = 0.0;
    adp->setHasLocalData(false);
    adp->setHasDecodedData(false);
    adp->setHasCompletedDecode(true);
    adp->setHasDecodeFailed(true);
    noteSoundIssue(adp, adp->getID(), ISSUE_MARKED_BAD,
                   llformat("%s after %u attempt(s); silent for %.0f s", why.c_str(), (U32)adp->mFailures, BAD_SOUND_TTL));
}

bool LLAudioEngine::expireBadMark(LLAudioData* adp, F64 now)
{
    if (!adp || adp->mBadUntil <= 0.0 || adp->mBadUntil > now)
    {
        return false;
    }
    // The bad mark ran out: the sound gets a fresh set of attempts (server-side fixes,
    // network recovery, a cache that is healthy again).
    LL_INFOS("AudioEngine") << "Sound " << adp->getID() << ": bad mark expired, will try again" << LL_ENDL;
    adp->mBadUntil = 0.0;
    adp->mFailures = 0;
    adp->mRetryAt = 0.0;
    adp->mReported = 0;
    adp->setHasDecodeFailed(false);
    adp->setHasCompletedDecode(false);
    adp->setHasWAVLoadFailed(false);
    adp->mRedecodeAttempted = false;
    adp->setHasDecodedData(hasDecodedFile(adp->getID()));
    adp->setHasLocalData(adp->hasDecodedData() || (gAssetStorage && gAssetStorage->hasLocalAsset(adp->getID(), LLAssetType::AT_SOUND)));
    if (adp->hasDecodedData())
    {
        adp->setHasCompletedDecode(true);
    }
    return true;
}

void LLAudioEngine::noteFailure(LLAudioData* adp, const std::string& why, bool not_retryable)
{
    if (!adp)
    {
        return;
    }
    if (adp->mFailures < 255)
    {
        ++adp->mFailures;
    }
    if (not_retryable || adp->mFailures >= MAX_SOUND_ATTEMPTS)
    {
        markBad(adp, why);
        return;
    }
    // Retry after a backoff. Nothing is "failed" meanwhile: sources keep waiting (their
    // loading age), and the transfer picker skips it until mRetryAt.
    const F64 backoff = adp->mFailures <= 1 ? 3.0 : 15.0;
    adp->mRetryAt = (F64)LLFrameTimer::getElapsedSeconds() + backoff;
    adp->setHasDecodeFailed(false);
    adp->setHasCompletedDecode(false);
    LL_DEBUGS("AudioEngine") << "Sound " << adp->getID() << ": " << why << ", attempt " << (U32)adp->mFailures
                             << " of " << (U32)MAX_SOUND_ATTEMPTS << ", retry in " << backoff << " s" << LL_ENDL;
}

void LLAudioEngine::noteFetchFailed(const LLUUID& id, const std::string& why, bool not_retryable)
{
    LLAudioData* adp = findAudioData(id);
    if (!adp)
    {
        ++mIssueCounts[ISSUE_FETCH_FAILED];
        mIssuesDirty = true;
        return;
    }
    adp->setHasLocalData(false);
    adp->setHasDecodedData(false);
    noteSoundIssue(adp, id, ISSUE_FETCH_FAILED,
                   llformat("%s; attempt %u of %u", why.c_str(), (U32)adp->mFailures + 1, (U32)MAX_SOUND_ATTEMPTS));
    noteFailure(adp, "fetch failed: " + why, not_retryable);
}

void LLAudioEngine::noteDecodeFailed(const LLUUID& id, const std::string& why)
{
    // The cached asset is suspect (a store record dropped as corrupt, a short read, bad
    // content): drop it so the retry fetches a fresh copy. LL kept it and marked the
    // sound failed for the session, so a transient cache problem silenced it for good.
    LLFileSystem::removeFile(id, LLAssetType::AT_SOUND);
    LLAudioData* adp = findAudioData(id);
    if (!adp)
    {
        ++mIssueCounts[ISSUE_DECODE_FAILED];
        mIssuesDirty = true;
        return;
    }
    adp->setHasLocalData(false);
    adp->setHasDecodedData(false);
    noteSoundIssue(adp, id, ISSUE_DECODE_FAILED,
                   llformat("%s; attempt %u of %u, refetching", why.c_str(), (U32)adp->mFailures + 1, (U32)MAX_SOUND_ATTEMPTS));
    noteFailure(adp, "decode failed: " + why, false);
}

void LLAudioEngine::noteDecodeSucceeded(const LLUUID& id)
{
    if (LLAudioData* adp = findAudioData(id))
    {
        adp->mFailures = 0;
        adp->mRetryAt = 0.0;
        adp->mReported = 0;     // a later failure is news again
    }
}

void LLAudioEngine::noteDecodedFilePresent(const LLUUID& id)
{
    LLAudioData* adp = findAudioData(id);
    if (adp && !adp->hasDecodedData() && !adp->isBad(LLFrameTimer::getElapsedSeconds()))
    {
        adp->setHasLocalData(true);
        adp->setHasDecodedData(true);
        adp->setHasCompletedDecode(true);
        adp->setHasDecodeFailed(false);
        adp->setHasWAVLoadFailed(false);
    }
}

void LLAudioEngine::advanceQueue(LLAudioSource* sourcep)
{
    LLAudioData* next = sourcep ? sourcep->mQueuedDatap : nullptr;
    if (!next)
    {
        return;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (next->isUnplayable(now))
    {
        // Out of retries: skip it rather than advance into silence (or wait forever).
        noteSoundIssue(next, next->getID(), ISSUE_QUEUED_BAD, "skipped in the queue");
        sourcep->mQueuedDatap = nullptr;
        return;
    }
    if (!next->getBuffer())
    {
        updateBufferForData(next, true);    // load if decoded, else decode first in line
    }
    if (!next->getBuffer())
    {
        // Not ready yet: keep it queued (that also keeps the source alive) and look again
        // next frame. LL advanced into the bufferless clip, played an empty channel,
        // flagged the source played and deleted it -- queue, preloads and all.
        return;
    }

    LLAudioData* cur = sourcep->mCurrentDatap;
    if (cur && cur != next && !sourcep->mPlayedOnce && !sourcep->isMuted()
        && !cur->isUnplayable(now))
    {
        // The current clip never started and we gave up waiting for it.
        noteSoundIssue(cur, cur->getID(), cur->getBuffer() ? ISSUE_NO_CHANNEL : ISSUE_NOT_READY,
                       "skipped for the next queued clip");
    }

    sourcep->mQueuedDatap = nullptr;
    sourcep->setCurrentData(next);
    if (sourcep->setupChannel())
    {
        LLAudioChannel* channelp = sourcep->getChannel();
        channelp->updateBuffer();
        channelp->play();
    }
    // else: no free channel this frame; idle() hands one to a source with a buffer.
}

void LLAudioEngine::noteSourceEnded(LLAudioSource* sourcep)
{
    LLAudioData* cur = sourcep ? sourcep->mCurrentDatap : nullptr;
    if (!cur || sourcep->mPlayedOnce || sourcep->mCorrupted)
    {
        return;     // it played, or the failure was reported where it happened
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (cur->isUnplayable(now))
    {
        return;
    }
    if (sourcep->isMuted())
    {
        noteSoundIssue(cur, cur->getID(), ISSUE_MUTED, "source muted, blocked, outside its cutoff radius or parcel audio");
    }
    else if (cur->getBuffer())
    {
        noteSoundIssue(cur, cur->getID(), ISSUE_NO_CHANNEL,
                       llformat("all %d channels held louder/closer sounds", LL_MAX_AUDIO_CHANNELS));
    }
    else
    {
        const char* state = isTransferInFlight(cur->getID()) ? "fetching"
                          : cur->mRetryAt > now ? "waiting to retry"
                          : cur->hasDecodedData() ? "loading"
                          : cur->hasLocalData() ? "decoding"
                          : "waiting for a fetch slot";
        // Exact state, so a stuck path is identifiable from the log alone.
        noteSoundIssue(cur, cur->getID(), ISSUE_NOT_READY,
                       llformat("still %s when given up; local=%d decoded=%d completed=%d failed=%d wav_failed=%d "
                                "decode_pending=%d decoded_file=%d buffer=%d failures=%u retry_in=%.1f",
                                state, (S32)cur->hasLocalData(), (S32)cur->hasDecodedData(),
                                (S32)cur->hasCompletedDecode(), (S32)cur->hasDecodeFailed(),
                                (S32)cur->hasWAVLoadFailed(),
                                (S32)LLAudioDecodeMgr::getInstance()->isPending(cur->getID()),
                                (S32)hasDecodedFile(cur->getID()), (S32)(cur->getBuffer() != nullptr),
                                (U32)cur->mFailures, llmax(0.0, cur->mRetryAt - now)));
    }
}

void LLAudioEngine::sweepAudioData()
{
    const F64 now = LLFrameTimer::getElapsedSeconds();

    // Everything that holds an LLAudioData* or will look one up by id. Built in the same
    // pass that frees, with no getAudioData() in between (it would re-create entries).
    std::unordered_set<const LLAudioData*> referenced;
    for (const source_map::value_type& sp : mAllSources)
    {
        const LLAudioSource* asp = sp.second;
        if (!asp)
        {
            continue;
        }
        referenced.insert(asp->mCurrentDatap);
        referenced.insert(asp->mQueuedDatap);
        for (const LLAudioSource::data_map::value_type& pp : asp->mPreloadMap)
        {
            referenced.insert(pp.second);
        }
    }
    for (const LLAudioBuffer* bufferp : mBuffers)
    {
        if (bufferp)
        {
            referenced.insert(bufferp->mAudioDatap);
        }
    }

    // Unused sounds are kept for a grace period (a music player replays its clips; a
    // looped sound restarts), less when over the soft cap.
    const F64 grace = mAllData.size() > AUDIO_DATA_SOFT_CAP ? 0.0 : 120.0;
    LLAudioDecodeMgr* decoder = LLAudioDecodeMgr::getInstance();
    for (data_map::iterator it = mAllData.begin(); it != mAllData.end();)
    {
        LLAudioData* adp = it->second;
        if (!adp)
        {
            it = mAllData.erase(it);
            continue;
        }
        expireBadMark(adp, now);
        if (referenced.count(adp)
            || adp->getBuffer()
            || adp->isBad(now)                      // the bad mark lives here until it expires
            || adp->mRetryAt > now                  // a retry is scheduled
            || now - adp->mLastTouch < grace
            || isTransferInFlight(adp->getID())
            || decoder->isPending(adp->getID()))
        {
            ++it;
            continue;
        }
        delete adp;
        it = mAllData.erase(it);
    }
}
