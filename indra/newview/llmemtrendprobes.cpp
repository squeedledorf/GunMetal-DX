/**
 * @file llmemtrendprobes.cpp
 * @brief The viewer's MemTrend probes (#883). See llmemtrendprobes.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "llviewerprecompiledheaders.h"

#include "llmemtrendprobes.h"

#include "llmemtrend.h"

#include "llaudiodecodemgr.h"
#include "llaudioengine.h"
#include "llcharacter.h"
#include "llcontrolavatar.h"
#include "llcoros.h"
#include "llerror.h"
#include "llevents.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterreg.h"
#include "llimage.h"
#include "llimview.h"
#include "llinventorymodel.h"
#include "llmeshrepository.h"
#include "llmutex.h"
#include "lltexturedata.h"
#include "llviewerobjectlist.h"
#include "llviewerpartsim.h"
#include "llviewerregion.h"
#include "llviewertexturelist.h"
#include "llworld.h"

// Byte figures are estimates (entry count x typical entry size) where the owner does not
// count bytes itself; the point is the trend, not the absolute number.
void registerMemTrendProbes()
{
    using LLMemTrend::Sample;
    using LLMemTrend::registerProbe;

    // ---- textures / images (the owners count their bytes) ----
    registerProbe("cpu-tex", []
    {
        Sample s;
        s.bytes = (U64)llmax(LLTextureData::getVKCpuBytes(), (S64)0);
        s.count = (U64)llmax(LLTextureData::getVKCpuCount(), 0);
        return s;
    });
    registerProbe("raw-images", []
    {
        Sample s;
        s.bytes = (U64)llmax(LLImageRaw::sRawImageBytes.load(), (S64)0);
        s.count = (U64)llmax(LLImageRaw::sRawImageCount.load(), 0);
        return s;
    });
    registerProbe("formatted-images", []
    {
        Sample s;
        s.bytes = (U64)llmax(LLImageFormatted::sGlobalFormattedMemory.load(), (S64)0);
        s.count = (U64)llmax(LLImageFormatted::sFormattedImageCount.load(), 0);
        return s;
    });
    registerProbe("viewer-textures", []
    {
        Sample s;
        s.count = (U64)llmax(gTextureList.getNumImages(), 0);
        s.bytes = s.count * 2048;   // LLViewerFetchedTexture + list / map nodes, pixels excluded
        return s;
    });

    // ---- world ----
    registerProbe("objects", []
    {
        Sample s;
        s.count = (U64)llmax(gObjectList.getNumObjects(), 0);
        s.bytes = s.count * 4096;   // LLViewerObject / LLVOVolume + drawable + TEs, rough
        return s;
    });
    registerProbe("characters", []
    {
        Sample s;
        s.count = LLCharacter::sInstances.size();   // avatars + animesh (control avatars)
        s.bytes = s.count * 256 * 1024;             // joints, motions, visual params, rough
        return s;
    });
    registerProbe("region-gltf-overrides", []
    {
        Sample s;
        if (LLWorld::instanceExists())
            for (const LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
                if (r) s.count += r->getGLTFOverrideCount();
        s.bytes = s.count * 2048;   // LLSD + LLGLTFMaterial per side, a few sides
        return s;
    });
    registerProbe("region-object-cache", []
    {
        Sample s;
        if (LLWorld::instanceExists())
            for (const LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
                if (r) s.count += r->getCacheMapCount();
        s.bytes = s.count * 1024;   // LLVOCacheEntry + its update buffer
        return s;
    });
    registerProbe("signaled-anims", []
    {
        Sample s;
        if (LLObjectSignaledAnimationMap::instanceExists())
            s.count = LLObjectSignaledAnimationMap::instance().getMap().size();
        s.bytes = s.count * 160;
        return s;
    });
    registerProbe("particle-sources", []
    {
        Sample s;
        if (LLViewerPartSim::instanceExists())
            if (const auto* l = LLViewerPartSim::getInstance()->getParticleSystemList())
                s.count = l->size();
        s.bytes = s.count * 512;
        return s;
    });

    // ---- mesh repository ----
    registerProbe("mesh-headers", []
    {
        Sample s;   // LLMeshRepoThread::mMeshHeader: one per mesh id ever fetched, never erased (LL)
        if (gMeshRepo.mThread && gMeshRepo.mThread->mHeaderMutex)
        {
            LLMutexLock lock(gMeshRepo.mThread->mHeaderMutex);
            s.count = gMeshRepo.mThread->mMeshHeader.size();
        }
        s.bytes = s.count * 200;
        return s;
    });
    registerProbe("mesh-skins", []
    {
        Sample s;
        s.count = gMeshRepo.mSkinMap.size();
        s.bytes = s.count * 8 * 1024;   // joint names + inverse bind matrices, typical skin
        return s;
    });
    registerProbe("mesh-decompositions", []
    {
        Sample s;
        s.count = gMeshRepo.mDecompositionMap.size();
        s.bytes = s.count * 16 * 1024;
        return s;
    });

    // ---- audio ----
    registerProbe("audio-data", []
    {
        Sample s;   // one LLAudioData per sound id in use; #891 frees unused ones (soft cap 4096)
        if (gAudiop) s.count = gAudiop->getAudioDataCount();
        s.bytes = s.count * 128;
        return s;
    });
    registerProbe("audio-sources", []
    {
        Sample s;
        if (gAudiop) s.count = gAudiop->getSourceCount();
        s.bytes = s.count * 512;
        return s;
    });
    registerProbe("audio-buffers", []
    {
        Sample s;   // decoded PCM lives in OpenAL; up to LL_MAX_AUDIO_BUFFERS
        if (gAudiop) s.count = gAudiop->getBufferCount();
        s.bytes = s.count * 1024 * 1024;   // ~1 MB of PCM for a typical clip
        return s;
    });
    registerProbe("audio-decode-queue", []
    {
        Sample s;
        if (LLAudioDecodeMgr::instanceExists())
            s.count = LLAudioDecodeMgr::instance().queuedCount() + LLAudioDecodeMgr::instance().inFlightCount();
        s.bytes = s.count * 32;
        return s;
    });
    registerProbe("audio-corrupt", []
    {
        Sample s;
        if (gAudiop) s.count = gAudiop->getCorruptCount();
        s.bytes = s.count * 48;
        return s;
    });

    // ---- chat / IM ----
    registerProbe("im-messages", []
    {
        Sample s;
        if (LLIMModel::instanceExists())
        {
            for (const auto& kv : LLIMModel::instance().mId2SessionMap)
            {
                if (!kv.second) continue;
                for (const LLIMModel::LLIMMessage& m : kv.second->mMsgs)
                {
                    ++s.count;
                    s.bytes += sizeof(m) + m.mFrom.capacity() + m.mText.capacity() + m.mTime.capacity();
                }
            }
        }
        return s;
    });
    registerProbe("nearby-chat-archive", []
    {
        Sample s;   // #369 windowed history keeps every line said this session
        if (LLFloaterIMNearbyChat* nc = LLFloaterReg::findTypedInstance<LLFloaterIMNearbyChat>("nearby_chat"))
            s.count = (U64)llmax(nc->getMessageArchiveLength(), 0);
        s.bytes = s.count * 400;
        return s;
    });

    // ---- infrastructure ----
    registerProbe("log-once-table", []
    {
        Sample s;
        s.count = LLError::uniqueLogMessageCount();
        s.bytes = s.count * 300;
        s.cap = LLError::uniqueLogMessageCap() * 300;
        return s;
    });
    registerProbe("coroutines", []
    {
        Sample s;
        s.count = LLCoros::coroCount();
        s.bytes = s.count * 256 * 1024;   // default coroutine stack
        return s;
    });
    registerProbe("event-pumps", []
    {
        Sample s;
        if (LLEventPumps::instanceExists()) s.count = LLEventPumps::instance().pumpCount();
        s.bytes = s.count * 512;
        return s;
    });
    registerProbe("inventory", []
    {
        Sample s;
        s.count = (U64)llmax(gInventory.getItemCount(), 0) + (U64)llmax(gInventory.getCategoryCount(), 0);
        s.bytes = s.count * 600;
        return s;
    });
}
