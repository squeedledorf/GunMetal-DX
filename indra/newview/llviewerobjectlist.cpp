/**
 * @file llviewerobjectlist.cpp
 * @brief Implementation of LLViewerObjectList class.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llviewerprecompiledheaders.h"
#include "llcrossingtrace.h"
#include "llcrossingkeep.h"

#include <limits>
#include "llviewerobjectlist.h"
#include "llworldclock.h"
#include "llvkhitch.h"
#include "llcleaningest.h"

#include "message.h"
#include "llfasttimer.h"
#include "llrender.h"
#include "llwindow.h"       // decBusyCount()

#include "llviewercontrol.h"
#include "llface.h"
#include "llvoavatar.h"
#include "llcontrolavatar.h"   // LLObjectSignaledAnimationMap
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llnetmap.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "pipeline.h"
#include "lltooltip.h"
#include "llworld.h"
#include "llstring.h"
#include "llhudicon.h"
#include "llhudnametag.h"
#include "lldrawable.h"
#include "llflexibleobject.h"
#include "llviewertextureanim.h"
#include "xform.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llselectmgr.h"
#include "llresmgr.h"
#include "llsdutil.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llviewerstatsrecorder.h"
#include "llvovolume.h"
#include "llcleaninstancestore.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "lltoolmgr.h"
#include "lltoolpie.h"
#include "llkeyboard.h"
#include "lllocalmesh.h"
#include "llmeshrepository.h"
#include "u64.h"
#include "llviewertexturelist.h"
#include "lldatapacker.h"
// [SL:KB] - Patch: World-Derender | Checked: 2011-12-15 (Catznip-3.2.1)
#include "llderenderlist.h"
// [/SL:KB]
#ifdef LL_USESYSTEMLIBS
#include <zlib.h>
#else
#include "zlib-ng/zlib.h"
#endif
#include "object_flags.h"

#include "llappviewer.h"
#include "llfloaterperms.h"
#include "llvocache.h"
#include "sldatausage.h"
#include "slprimdefaults.h"
#include "llcorehttputil.h"
#include "llstartup.h"
#include "fsareasearch.h"
#include "llfloaterreg.h"

#include <algorithm>
#include <iterator>

extern F32 gMinObjectDistance;

#define MAX_CONCURRENT_PHYSICS_REQUESTS 256

void dialog_refresh_all();

// Global lists of objects - should go away soon.
LLViewerObjectList gObjectList;

extern LLPipeline   gPipeline;

// Statics for object lookup tables.
U32                     LLViewerObjectList::sSimulatorMachineIndex = 1; // Not zero deliberately, to speed up index check.

LLViewerObjectList::LLViewerObjectList()
{
    mCurLazyUpdateIndex = 0;
    mCurBin = 0;
    mNumDeadObjects = 0;
    mNumOrphans = 0;
    mNumNewObjects = 0;
    mWasPaused = false;
    mNumDeadObjectUpdates = 0;
    mNumUnknownUpdates = 0;
}

LLViewerObjectList::~LLViewerObjectList()
{
    destroy();
}

void LLViewerObjectList::destroy()
{
    killAllObjects();
    LLCrossingKeep::clear();   // #607

    resetObjectBeacons();
    mActiveObjects.clear();
    mDeadObjects.clear();
    mMapObjects.clear();
    mUUIDObjectMap.clear();
}


void LLViewerObjectList::getUUIDFromLocal(LLUUID &id,
                                          const U32 local_id,
                                          const U32 ip,
                                          const U32 port)
{
    U64 ipport = (((U64)ip) << 32) | (U64)port;

    U32 index = mIPAndPortToIndex[ipport];

    if (!index)
    {
        index = sSimulatorMachineIndex++;
        mIPAndPortToIndex[ipport] = index;
    }

    U64 indexid = (((U64)index) << 32) | (U64)local_id;

    id = get_if_there(mIndexAndLocalIDToUUID, indexid, LLUUID::null);
}

U64 LLViewerObjectList::getIndex(const U32 local_id,
                                 const U32 ip,
                                 const U32 port)
{
    U64 ipport = (((U64)ip) << 32) | (U64)port;

    U32 index = mIPAndPortToIndex[ipport];

    if (!index)
    {
        return 0;
    }

    return (((U64)index) << 32) | (U64)local_id;
}

bool LLViewerObjectList::removeFromLocalIDTable(LLViewerObject* objectp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    if(objectp && objectp->mRegionIndex != 0)
    {
        U32 local_id = objectp->mLocalID;
        U64 indexid = (((U64)objectp->mRegionIndex) << 32) | (U64)local_id;

        auto iter = mIndexAndLocalIDToUUID.find(indexid);
        if (iter == mIndexAndLocalIDToUUID.end())
        {
            return false;
        }

        // Found existing entry
        if (iter->second == objectp->getID())
        {   // Full UUIDs match, so remove the entry
            mIndexAndLocalIDToUUID.erase(iter);
            objectp->mRegionIndex = 0;
            return true;
        }
        // UUIDs did not match - this would zap a valid entry, so don't erase it
        //LL_INFOS() << "Tried to erase entry where id in table ("
        //      << iter->second << ") did not match object " << object.getID() << LL_ENDL;
    }

    return false ;
}

void LLViewerObjectList::setUUIDAndLocal(const LLUUID &id,
                                          const U32 local_id,
                                          const U32 ip,
                                          const U32 port,
                                          LLViewerObject* objectp)
{
    U64 ipport = (((U64)ip) << 32) | (U64)port;

    U32 index = mIPAndPortToIndex[ipport];

    if (!index)
    {
        index = sSimulatorMachineIndex++;
        mIPAndPortToIndex[ipport] = index;
    }

    objectp->mRegionIndex = index; // should never be zero, sSimulatorMachineIndex starts from 1
    U64 indexid = (((U64)index) << 32) | (U64)local_id;

    mIndexAndLocalIDToUUID[indexid] = id;

    //LL_INFOS() << "Adding object to table, full ID " << id
    //  << ", local ID " << local_id << ", ip " << ip << ":" << port << LL_ENDL;
}

S32 gFullObjectUpdates = 0;
S32 gTerseObjectUpdates = 0;

void LLViewerObjectList::processUpdateCore(LLViewerObject* objectp,
                                           void** user_data,
                                           U32 i,
                                           const EObjectUpdateType update_type,
                                           LLDataPacker* dpp,
                                           bool just_created,
                                           bool from_cache)
{
    LLMessageSystem* msg = NULL;

    if(!from_cache)
    {
        msg = gMessageSystem;
    }

    // ignore returned flags
    LL_DEBUGS("ObjectUpdate") << "uuid " << objectp->mID << " calling processUpdateMessage "
                              << objectp << " just_created " << just_created << " from_cache " << from_cache << " msg " << msg << LL_ENDL;

    objectp->processUpdateMessage(msg, user_data, i, update_type, dpp);
    LLCrossingTrace::onObjectUpdate(objectp);   // #568

    if (objectp->isDead())
    {
        // The update failed
        return;
    }

    updateActive(objectp);

    if (just_created)
    {
        gPipeline.addObject(objectp);
    }

    // Also sets the approx. pixel area
    objectp->setPixelAreaAndAngle(gAgent);

    // RN: this must be called after we have a drawable
    // (from gPipeline.addObject)
    // so that the drawable parent is set properly
    if(msg != NULL)
    {
    findOrphans(objectp, msg->getSenderIP(), msg->getSenderPort());
    }
    else
    {
        LLViewerRegion* regionp = objectp->getRegion();
        if(regionp != NULL)
        {
            findOrphans(objectp, regionp->getHost().getAddress(), regionp->getHost().getPort());
        }
    }

    // If we're just wandering around, don't create new objects selected.
    if (just_created
        && update_type != OUT_TERSE_IMPROVED
        && objectp->mCreateSelected)
    {
        // Preferences > Building defaults for a prim we just rezzed (#466)
        if (SLPrimDefaults::instanceExists())
        {
            SLPrimDefaults::getInstance()->onCreateSelectedObject(objectp);
        }

        if ( LLToolMgr::getInstance()->getCurrentTool() != LLToolPie::getInstance() )
        {
            // LL_INFOS() << "DEBUG selecting " << objectp->mID << " "
            // << objectp->mLocalID << LL_ENDL;
            LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);
            dialog_refresh_all();
        }

        objectp->mCreateSelected = false;
        gViewerWindow->getWindow()->decBusyCount();
        gViewerWindow->setCursor( UI_CURSOR_ARROW );
    }
}

static LLTrace::BlockTimerStatHandle FTM_PROCESS_OBJECTS("Process Objects");

LLViewerObject* LLViewerObjectList::processObjectUpdateFromCache(LLVOCacheEntry* entry, LLViewerRegion* regionp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    LLDataPacker *cached_dpp = entry->getDP();

    if (!cached_dpp || gNonInteractive)
    {
        return NULL; //nothing cached.
    }

    LLViewerObject *objectp;
    U32             local_id;
    LLPCode         pcode = 0;
    LLUUID          fullid;
    LLViewerStatsRecorder& recorder = LLViewerStatsRecorder::instance();

    // Cache Hit.
    record(LLStatViewer::OBJECT_CACHE_HIT_RATE, LLUnits::Ratio::fromValue(1));

    cached_dpp->reset();
    cached_dpp->unpackUUID(fullid, "ID");
    cached_dpp->unpackU32(local_id, "LocalID");
    cached_dpp->unpackU8(pcode, "PCode");

    objectp = findObject(fullid);

    if (objectp)
    {
        if(!objectp->isDead() && (objectp->mLocalID != entry->getLocalID() ||
            objectp->getRegion() != regionp))
        {
            removeFromLocalIDTable(objectp);
            setUUIDAndLocal(fullid, entry->getLocalID(),
                            regionp->getHost().getAddress(),
                            regionp->getHost().getPort(),
                            objectp);

            if (objectp->mLocalID != entry->getLocalID())
            {   // Update local ID in object with the one sent from the region
                objectp->mLocalID = entry->getLocalID();
            }

            if (objectp->getRegion() != regionp)
            {   // Object changed region, so update it
                objectp->updateRegion(regionp); // for LLVOAvatar
            }
            LLCrossingKeep::onRekey(objectp);   // #607: cancels a deferred kill
        }
        else
        {
            //should fall through if already loaded because may need to update the object.
            //return objectp; //already loaded.
        }
    }

// [SL:KB] - Patch: World-Derender | Checked: 2014-08-10 (Catznip-3.7)
    // Don't recreate derendered objects (also kill the cache entry so we don't do this per-frame)
    if (LLDerenderList::instance().processObjectUpdate(regionp->getHandle(), fullid, entry))
    {
        regionp->killCacheEntry(local_id);  // NOTE: this will kill all child entries from the cache as well
        return NULL;
    }
// [/SL:KB]

    bool justCreated = false;
    if (!objectp)
    {
        objectp = createObjectFromCache(pcode, regionp, fullid, entry->getLocalID());

        LL_DEBUGS("ObjectUpdate") << "uuid " << fullid << " created objectp " << objectp << LL_ENDL;

        if (!objectp)
        {
            LL_INFOS() << "createObject failure for object: " << fullid << LL_ENDL;
            recorder.objectUpdateFailure();
            return NULL;
        }
        justCreated = true;
        mNumNewObjects++;
    }

    if (objectp->isDead())
    {
        LL_WARNS() << "Dead object " << objectp->mID << " in UUID map 1!" << LL_ENDL;
    }

    processUpdateCore(objectp, NULL, 0, OUT_FULL_CACHED, cached_dpp, justCreated, true);
    objectp->loadFlags(entry->getUpdateFlags()); //just in case, reload update flags from cache.

    if(entry->getHitCount() > 0)
    {
        objectp->setLastUpdateType(OUT_FULL_CACHED);
    }
    else
    {
        objectp->setLastUpdateType(OUT_FULL_COMPRESSED); //newly cached
        objectp->setLastUpdateCached(true);
    }

    return objectp;
}

void LLViewerObjectList::processObjectUpdate(LLMessageSystem *mesgsys,
                                             void **user_data,
                                             const EObjectUpdateType update_type,
                                             bool compressed)
{
    LL_RECORD_BLOCK_TIME(FTM_PROCESS_OBJECTS);

    LLViewerObject *objectp;
    S32         num_objects;
    U32         local_id = 0;
    LLPCode     pcode = 0;
    LLUUID      fullid;
    S32         i;

    // figure out which simulator these are from and get it's index
    // Coordinates in simulators are region-local
    // Until we get region-locality working on viewer we
    // have to transform to absolute coordinates.
    num_objects = mesgsys->getNumberOfBlocksFast(_PREHASH_ObjectData);

    // I don't think this case is ever hit.  TODO* Test this.
    if (!compressed && update_type != OUT_FULL)
    {
        //LL_INFOS() << "TEST: !cached && !compressed && update_type != OUT_FULL" << LL_ENDL;
        gTerseObjectUpdates += num_objects;
        /*
        S32 size;
        if (mesgsys->getReceiveCompressedSize())
        {
            size = mesgsys->getReceiveCompressedSize();
        }
        else
        {
            size = mesgsys->getReceiveSize();
        }
        LL_INFOS() << "Received terse " << num_objects << " in " << size << " byte (" << size/num_objects << ")" << LL_ENDL;
        */
    }
    else
    {
        /*
        S32 size;
        if (mesgsys->getReceiveCompressedSize())
        {
            size = mesgsys->getReceiveCompressedSize();
        }
        else
        {
            size = mesgsys->getReceiveSize();
        }

        LL_INFOS() << "Received " << num_objects << " in " << size << " byte (" << size/num_objects << ")" << LL_ENDL;
        */
        gFullObjectUpdates += num_objects;
    }

    U64 region_handle;
    mesgsys->getU64Fast(_PREHASH_RegionData, _PREHASH_RegionHandle, region_handle);

    LLViewerRegion *regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);

    if (!regionp)
    {
        LL_WARNS() << "Object update from unknown region! " << region_handle << LL_ENDL;
        return;
    }

    // A variable block can be as large as the packet that carried it, and the
    // message reader never yields a packet larger than MAX_BUFFER_SIZE.
    // (Port of upstream 6988483ffc: a 2049..8180 byte block used to be decoded
    // past a 2048-byte stack buffer and the stray stack bytes cached to disk.)
    U8 compressed_dpbuffer[MAX_BUFFER_SIZE];
    LLDataPackerBinaryBuffer compressed_dp(compressed_dpbuffer, sizeof(compressed_dpbuffer));
    LLViewerStatsRecorder& recorder = LLViewerStatsRecorder::instance();

    for (i = 0; i < num_objects; i++)
    {
        bool justCreated = false;
        bool update_cache = false; //update object cache if it is a full-update or terse update

        if (compressed)
        {
            compressed_dp.reset();

            // getSizeFast returns a negative sentinel for a missing block or
            // variable, and getBinaryDataFast silently truncates at max_size.
            // The packer's bounds checks trust the size it is handed, so a
            // block we cannot hold whole must not be decoded at all.
            S32 uncompressed_length = mesgsys->getSizeFast(_PREHASH_ObjectData, i, _PREHASH_Data);
            if (uncompressed_length < 0 || uncompressed_length > (S32)sizeof(compressed_dpbuffer))
            {
                LL_WARNS() << "Object data block " << i << " declares " << uncompressed_length
                    << " bytes against a " << sizeof(compressed_dpbuffer) << " byte buffer"
                    << " from " << mesgsys->getSender() << ", skipping" << LL_ENDL;
                recorder.objectUpdateFailure();
                continue;
            }
            LL_DEBUGS("ObjectUpdate") << "got binary data from message to compressed_dpbuffer" << LL_ENDL;
            mesgsys->getBinaryDataFast(_PREHASH_ObjectData, _PREHASH_Data, compressed_dpbuffer, 0, i, (S32)sizeof(compressed_dpbuffer));
            compressed_dp.assignBuffer(compressed_dpbuffer, uncompressed_length);

            if (update_type != OUT_TERSE_IMPROVED) // OUT_FULL_COMPRESSED only?
            {
                U32 flags = 0;
                mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_UpdateFlags, flags, i);

                compressed_dp.unpackUUID(fullid, "ID");
                compressed_dp.unpackU32(local_id, "LocalID");
                compressed_dp.unpackU8(pcode, "PCode");

                if (pcode == 0)
                {
                    // object creation will fail, LLViewerObject::createObject()
                    LL_WARNS() << "Received object " << fullid
                        << " with 0 PCode. Local id: " << local_id
                        << " Flags: " << flags
                        << " Region: " << regionp->getName()
                        << " Region id: " << regionp->getRegionID() << LL_ENDL;
                    recorder.objectUpdateFailure();
                    continue;
                }
                else if ((flags & FLAGS_TEMPORARY_ON_REZ) == 0)
                {
                    //send to object cache
                    regionp->cacheFullUpdate(compressed_dp, flags);
                    continue;
                }
            }
            else //OUT_TERSE_IMPROVED
            {
                update_cache = true;
                compressed_dp.unpackU32(local_id, "LocalID");
                getUUIDFromLocal(fullid,
                                 local_id,
                                 gMessageSystem->getSenderIP(),
                                 gMessageSystem->getSenderPort());
                if (fullid.isNull())
                {
                    LL_DEBUGS() << "update for unknown localid " << local_id << " host " << gMessageSystem->getSender() << ":" << gMessageSystem->getSenderPort() << LL_ENDL;
                    mNumUnknownUpdates++;
                }
            }
        }
        else if (update_type != OUT_FULL) // !compressed, !OUT_FULL ==> OUT_FULL_CACHED only?
        {
            mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_ID, local_id, i);

            getUUIDFromLocal(fullid,
                            local_id,
                            gMessageSystem->getSenderIP(),
                            gMessageSystem->getSenderPort());
            if (fullid.isNull())
            {
                // LL_WARNS() << "update for unknown localid " << local_id << " host " << gMessageSystem->getSender() << LL_ENDL;
                mNumUnknownUpdates++;
            }
            else
            {
                LL_DEBUGS("ObjectUpdate") << "Non-full, non-compressed update, obj " << local_id << ", global ID " << fullid << " from " << mesgsys->getSender() << LL_ENDL;
            }
        }
        else // OUT_FULL only?
        {
            update_cache = true;
            mesgsys->getUUIDFast(_PREHASH_ObjectData, _PREHASH_FullID, fullid, i);
            mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_ID, local_id, i);
            LL_DEBUGS("ObjectUpdate") << "Full Update, obj " << local_id << ", global ID " << fullid << " from " << mesgsys->getSender() << LL_ENDL;
        }
        objectp = findObject(fullid);

        if (compressed)
        {
            LL_DEBUGS("ObjectUpdate") << "uuid " << fullid << " received compressed data from message (earlier in function)" << LL_ENDL;
        }
        LL_DEBUGS("ObjectUpdate") << "uuid " << fullid << " objectp " << objectp
                                     << " update_cache " << (S32) update_cache << " compressed " << compressed
                                     << " update_type "  << update_type << LL_ENDL;

        if(update_cache)
        {
            //update object cache if the object receives a full-update or terse update
            objectp = regionp->updateCacheEntry(local_id, objectp);
        }

        // This looks like it will break if the local_id of the object doesn't change
        // upon boundary crossing, but we check for region id matching later...
        // Reset object local id and region pointer if things have changed
        if (objectp &&
            ((objectp->mLocalID != local_id) ||
             (objectp->getRegion() != regionp)))
        {
            //if (objectp->getRegion())
            //{
            //  LL_INFOS() << "Local ID change: Removing object from table, local ID " << objectp->mLocalID
            //          << ", id from message " << local_id << ", from "
            //          << LLHost(objectp->getRegion()->getHost().getAddress(), objectp->getRegion()->getHost().getPort())
            //          << ", full id " << fullid
            //          << ", objects id " << objectp->getID()
            //          << ", regionp " << (U32) regionp << ", object region " << (U32) objectp->getRegion()
            //          << LL_ENDL;
            //}
            removeFromLocalIDTable(objectp);
            setUUIDAndLocal(fullid,
                            local_id,
                            gMessageSystem->getSenderIP(),
                            gMessageSystem->getSenderPort(),
                            objectp);

            if (objectp->mLocalID != local_id)
            {   // Update local ID in object with the one sent from the region
                objectp->mLocalID = local_id;
            }

            if (objectp->getRegion() != regionp)
            {   // Object changed region, so update it
                objectp->updateRegion(regionp); // for LLVOAvatar
            }
            LLCrossingKeep::onRekey(objectp);   // #607: cancels a deferred kill
        }

        if (!objectp)
        {
            if (compressed)
            {
                if (update_type == OUT_TERSE_IMPROVED)
                {
                    // LL_INFOS() << "terse update for an unknown object (compressed):" << fullid << LL_ENDL;
                    recorder.objectUpdateFailure();
                    continue;
                }
            }
            else
            {
                if (update_type != OUT_FULL)
                {
                    //LL_INFOS() << "terse update for an unknown object:" << fullid << LL_ENDL;
                    recorder.objectUpdateFailure();
                    continue;
                }

                mesgsys->getU8Fast(_PREHASH_ObjectData, _PREHASH_PCode, pcode, i);

            }
#ifdef IGNORE_DEAD
            if (mDeadObjects.find(fullid) != mDeadObjects.end())
            {
                mNumDeadObjectUpdates++;
                //LL_INFOS() << "update for a dead object:" << fullid << LL_ENDL;
                recorder.objectUpdateFailure();
                continue;
            }
#endif

// [SL:KB] - Patch: World-Derender | Checked: 2012-06-08 (Catznip-3.3)
            // Don't recreate derendered objects (update the core object information so we'll have enough information to rerequest it later if needed)
            if ( (OUT_FULL == update_type) || (OUT_FULL_COMPRESSED == update_type) )
            {
                bool fBlockObject = false;
                if (OUT_FULL == update_type)
                {
                    U32 idRootLocal = 0;
                    mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_ParentID, idRootLocal, i);
                    fBlockObject = LLDerenderList::instance().processObjectUpdate(regionp->getHandle(), fullid, local_id, idRootLocal, LL_PCODE_LEGACY_AVATAR == pcode);
                }
                else if (OUT_FULL_COMPRESSED == update_type)
                {
                    fBlockObject = LLDerenderList::instance().processObjectUpdate(regionp->getHandle(), fullid, local_id, compressed_dp.getBuffer(), LL_PCODE_LEGACY_AVATAR == pcode);
                }

                if (fBlockObject)
                    continue;
            }
// [/SL:KB]

            objectp = createObject(pcode, regionp, fullid, local_id, gMessageSystem->getSender());

            LL_DEBUGS("ObjectUpdate") << "creating object " << fullid << " result " << objectp << LL_ENDL;

            if (!objectp)
            {
                LL_INFOS() << "createObject failure for object: " << fullid << LL_ENDL;
                recorder.objectUpdateFailure();
                continue;
            }

            justCreated = true;
            mNumNewObjects++;
        }

        if (objectp->isDead())
        {
            LL_WARNS() << "Dead object " << objectp->mID << " in UUID map 1!" << LL_ENDL;
        }

        //bool bCached = false;
        if (compressed)
        {
            if (update_type != OUT_TERSE_IMPROVED) // OUT_FULL_COMPRESSED only?
            {
                objectp->mLocalID = local_id;
            }
            processUpdateCore(objectp, user_data, i, update_type, &compressed_dp, justCreated);

#if 0
            if (update_type != OUT_TERSE_IMPROVED) // OUT_FULL_COMPRESSED only?
            {
                U32 flags = 0;
                mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_UpdateFlags, flags, i);

                if(!(flags & FLAGS_TEMPORARY_ON_REZ))
                {
                    bCached = true;
                    LLViewerRegion::eCacheUpdateResult result = objectp->mRegionp->cacheFullUpdate(objectp, compressed_dp, flags);
                    recorder.cacheFullUpdate(result);
                }
            }
#endif
        }
        else
        {
            if (update_type == OUT_FULL)
            {
                objectp->mLocalID = local_id;
            }
            processUpdateCore(objectp, user_data, i, update_type, NULL, justCreated);
        }
        recorder.objectUpdateEvent(update_type);
        SLDataUsage::noteObjectUpdate(objectp, (S32)update_type);   // #528/#529: who the sim keeps updating
        objectp->setLastUpdateType(update_type);
    }
}

void LLViewerObjectList::processCompressedObjectUpdate(LLMessageSystem *mesgsys,
                                             void **user_data,
                                             const EObjectUpdateType update_type)
{
    processObjectUpdate(mesgsys, user_data, update_type, true);
}

void LLViewerObjectList::processCachedObjectUpdate(LLMessageSystem *mesgsys,
                                             void **user_data,
                                             const EObjectUpdateType update_type)
{
    //processObjectUpdate(mesgsys, user_data, update_type, true, false);

    S32 num_objects = mesgsys->getNumberOfBlocksFast(_PREHASH_ObjectData);
    gFullObjectUpdates += num_objects;

    U64 region_handle;
    mesgsys->getU64Fast(_PREHASH_RegionData, _PREHASH_RegionHandle, region_handle);
    LLViewerRegion *regionp = LLWorld::getInstance()->getRegionFromHandle(region_handle);
    if (!regionp)
    {
        LL_WARNS() << "Object update from unknown region! " << region_handle << LL_ENDL;
        return;
    }

    LLViewerStatsRecorder& recorder = LLViewerStatsRecorder::instance();

    for (S32 i = 0; i < num_objects; i++)
    {
        U32 id;
        U32 crc;
        U32 flags;
        mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_ID, id, i);
        mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_CRC, crc, i);
        mesgsys->getU32Fast(_PREHASH_ObjectData, _PREHASH_UpdateFlags, flags, i);

        LL_DEBUGS("ObjectUpdate") << "got probe for id " << id << " crc " << crc << LL_ENDL;

        // Lookup data packer and add this id to cache miss lists if necessary.
        U8 cache_miss_type = LLViewerRegion::CACHE_MISS_TYPE_NONE;
        if (regionp->probeCache(id, crc, flags, cache_miss_type))
        {   // Cache Hit
            recorder.cacheHitEvent();
            SLDataUsage::noteObjectCacheProbe(true, false);     // #529
        }
        else
        {   // Cache Miss
            LL_DEBUGS("ObjectUpdate") << "cache miss for id " << id << " crc " << crc << " miss type " << (S32) cache_miss_type << LL_ENDL;
            recorder.cacheMissEvent(cache_miss_type);
            SLDataUsage::noteObjectCacheProbe(false, cache_miss_type == LLViewerRegion::CACHE_MISS_TYPE_CRC);    // #529
        }
    }

    return;
}

void LLViewerObjectList::dirtyAllObjectInventory()
{
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        (*iter)->dirtyInventory();
    }
}

// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
void LLViewerObjectList::setAllObjectDefaultTextures(U32 nChannel, bool fShowDefault)
{
    LLPipeline::sRenderTextures = !fShowDefault;

    for (LLViewerObject* pObj : mObjects)
    {
        LLDrawable* pDrawable = pObj->mDrawable;
        if ( (pDrawable) && (!pDrawable->isDead()) )
        {
            for (int idxFace = 0, cntFace = pDrawable->getNumFaces(); idxFace < cntFace; idxFace++)
            {
                if (LLFace* pFace = pDrawable->getFace(idxFace))
                    pFace->setDefaultTexture(nChannel, fShowDefault);
            }

            if (LLVOVolume* pVoVolume = pDrawable->getVOVolume())
                pVoVolume->markForUpdate();
        }
    }
}
// [/SL:KB]

void LLViewerObjectList::updateApparentAngles(LLAgent &agent)
{
    S32 i;
    LLViewerObject *objectp;

    S32 num_updates, max_value;
    if (NUM_BINS - 1 == mCurBin)
    {
        // Remainder (mObjects.size() could have changed)
        num_updates = (S32) mObjects.size() - mCurLazyUpdateIndex;
        max_value = (S32) mObjects.size();
    }
    else
    {
        num_updates = ((S32) mObjects.size() / NUM_BINS) + 1;
        max_value = llmin((S32) mObjects.size(), mCurLazyUpdateIndex + num_updates);
    }

    // Iterate through some of the objects and lazy update their texture priorities
    for (i = mCurLazyUpdateIndex; i < max_value; i++)
    {
        objectp = mObjects[i];
        if (!objectp->isDead())
        {
            //  Update distance & gpw
            objectp->setPixelAreaAndAngle(agent); // Also sets the approx. pixel area
            objectp->updateTextures();  // Update the image levels of textures for this object.
        }
    }

    mCurLazyUpdateIndex = max_value;
    if (mCurLazyUpdateIndex == mObjects.size())
    {
        // restart
        mCurLazyUpdateIndex = 0;
        mCurBin = 0; // keep in sync with index (mObjects.size() could have changed)
    }
    else
    {
        mCurBin = (mCurBin + 1) % NUM_BINS;
    }

#if 0
    // Slam priorities for textures that we care about (hovered, selected, and focused)
    // Hovered
    // Assumes only one level deep of parenting
    LLSelectNode* nodep = LLSelectMgr::instance().getHoverNode();
    if (nodep)
    {
        objectp = nodep->getObject();
        if (objectp)
        {
            objectp->boostTexturePriority();
        }
    }

    // Focused
    objectp = gAgentCamera.getFocusObject();
    if (objectp)
    {
        objectp->boostTexturePriority();
    }
#endif

    // Selected
    struct f : public LLSelectedObjectFunctor
    {
        virtual bool apply(LLViewerObject* objectp)
        {
            if (objectp)
            {
                objectp->boostTexturePriority();
            }
            return true;
        }
    } func;
    LLSelectMgr::getInstance()->getSelection()->applyToRootObjects(&func);

    LLVOAvatar::cullAvatarsByPixelArea();
}

void LLViewerObjectList::update(LLAgent &agent)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    static LLCachedControl<bool> cc_velocity_interpolate(gSavedSettings, "VelocityInterpolate");
    static LLCachedControl<bool> cc_ping_interpolate(gSavedSettings, "PingInterpolate");
    static LLCachedControl<F32> cc_interpolation_time(gSavedSettings, "InterpolationTime");
    static LLCachedControl<F32> cc_ping_region_cross_interp(gSavedSettings, "RegionCrossingInterpolationTime");
    static LLCachedControl<F32> cc_interpolation_phase_out(gSavedSettings, "InterpolationPhaseOut");

    LLCrossingKeep::tick();   // #607: apply expired / pressure-flushed deferred kills

    // Update globals
    LLViewerObject::setVelocityInterpolate( cc_velocity_interpolate );
    LLViewerObject::setPingInterpolate( cc_ping_interpolate );

    F32 interp_time = cc_interpolation_time;
    F32 phase_out_time = cc_interpolation_phase_out;
    F32 region_interp_time = llclamp(cc_ping_region_cross_interp(), 0.5f, 5.f);
    if (interp_time < 0.0f ||
        phase_out_time < 0.0f ||
        phase_out_time > interp_time)
    {
        LL_WARNS() << "Invalid values for InterpolationTime or InterpolationPhaseOut, resetting to defaults" << LL_ENDL;
        interp_time = 3.0f;
        phase_out_time = 1.0f;
    }
    LLViewerObject::setPhaseOutUpdateInterpolationTime( phase_out_time );
    LLViewerObject::setMaxUpdateInterpolationTime( interp_time );
    LLViewerObject::setMaxRegionCrossingInterpolationTime(region_interp_time);

    // gFrameTime / gFrameTimeSeconds / gFrameIntervalSeconds and the world clock are
    // advanced by LLAppViewer::idle() (updateFrameClock) before network processing.
    // Time between consecutive object-list updates, for frame stats (frames that skip
    // this update, e.g. teleporting, count toward the next one like they always did).
    static U64Microseconds sLastUpdateFrameTime(gFrameTime);
    const U64Microseconds frame_now(gFrameTime);
    F64Seconds time_diff = frame_now - sLastUpdateFrameTime;
    sLastUpdateFrameTime = frame_now;

    //clear avatar LOD change counter
    LLVOAvatar::sNumLODChangesThisFrame = 0;

    const F64 frame_time = LLFrameTimer::getElapsedSeconds();

    LLViewerObject *objectp = NULL;

    // Make a copy of the list in case something in idleUpdate() messes with it
    static std::vector<LLViewerObject*> idle_list;

    U32 idle_count = 0;
    mNumAvatars = 0;

    {
        for (std::vector<LLPointer<LLViewerObject> >::iterator active_iter = mActiveObjects.begin();
            active_iter != mActiveObjects.end(); active_iter++)
        {
            objectp = *active_iter;
            if (objectp)
            {
                if (idle_count >= idle_list.size())
                {
                    idle_list.push_back( objectp );
                }
            else
                {
                    idle_list[idle_count] = objectp;
                }
                ++idle_count;
                if (objectp->isAvatar())
                {
                    mNumAvatars++;
                }
            }
            else
            {   // There shouldn't be any NULL pointers in the list, but they have caused
                // crashes before.  This may be idleUpdate() messing with the list.
                LL_WARNS() << "LLViewerObjectList::update has a NULL objectp" << LL_ENDL;
            }
        }
    }

    std::vector<LLViewerObject*>::iterator idle_end = idle_list.begin()+idle_count;

    // World frozen (Freeze World / FreezeTime): only avatars update (own movement, the
    // camera anchor, network keepalive of avatar state). Everything else holds its
    // interpolation clock, or the thaw frame would apply the whole pause as one
    // velocity / omega step.
    if (LLWorldClock::isFrozen())
    {

        for (std::vector<LLViewerObject*>::iterator iter = idle_list.begin();
            iter != idle_end; iter++)
        {
            objectp = *iter;
            if (objectp->isAvatar())
            {
                objectp->idleUpdate(agent, frame_time);
            }
            else
            {
                objectp->holdInterpolation(frame_time);
            }
        }
    }
    else
    {
        // Two passes: every non-avatar first, then avatars. A seated avatar's
        // idleUpdate early-moves its seat's drawable (updateMoveDampedAsync) and then
        // poses itself (and the camera follows) from that drawable. In one pass the
        // seat interpolated before OR after its avatar depending on list order, which
        // shifts as the active set changes while driving: the vehicle, driver and
        // camera alternated between the previous and the current position -> one
        // frozen frame then a double step, ~20% of frames (#96).
        {
        VK_HITCH_SCOPE("objUpd_nonAv");
        for (std::vector<LLViewerObject*>::iterator idle_iter = idle_list.begin();
            idle_iter != idle_end; idle_iter++)
        {
            objectp = *idle_iter;
            llassert(objectp->isActive());
            if (!objectp->isAvatar())
                objectp->idleUpdate(agent, frame_time);
        }
        }
        LLVKHitch::count("activeObjects", idle_count);
        LLVKHitch::count("activeAvatars", mNumAvatars);
        // Avatars, then ATTACHED ANIMESH last. An animesh worn as an attachment poses
        // its control avatar from the wearer's attachment point
        // (LLControlAvatar::matchVolumeTransform, run by its updateCharacter): in one
        // pass, a control avatar that sorted before its wearer in mActiveObjects read
        // the wearer's PREVIOUS-frame skeleton and trailed the body by a frame -- most
        // visible in fast animations (order shifts as the active set changes, so the
        // lag came and went). The wearer's updateCharacter must run first.
        {
        VK_HITCH_SCOPE("objUpd_av");
        static std::vector<LLViewerObject*> s_attached_animesh;
        s_attached_animesh.clear();
        for (std::vector<LLViewerObject*>::iterator idle_iter = idle_list.begin();
            idle_iter != idle_end; idle_iter++)
        {
            objectp = *idle_iter;
            if (!objectp->isAvatar())
                continue;
            LLVOAvatar* avp = static_cast<LLVOAvatar*>(objectp);
            if (avp->isControlAvatar() && static_cast<LLControlAvatar*>(avp)->getAttachedAvatar())
            {
                s_attached_animesh.push_back(objectp);
                continue;
            }
            objectp->idleUpdate(agent, frame_time);
        }
        for (LLViewerObject* animesh : s_attached_animesh)
            animesh->idleUpdate(agent, frame_time);
        s_attached_animesh.clear();
        }
    }



    { VK_HITCH_SCOPE("objUpd_costs"); fetchObjectCosts();
    fetchPhysicsFlags(); }

    // update max computed render cost
    { VK_HITCH_SCOPE("objUpd_complexity"); LLVOVolume::updateRenderComplexity(); }

    // compute all sorts of time-based stats
    // don't factor frames that were paused into the stats
    if (! mWasPaused)
    {
        LLViewerStats::getInstance()->updateFrameStats(time_diff);
    }

    /*
    // Debugging code for viewing orphans, and orphaned parents
    LLUUID id;
    for (i = 0; i < mOrphanParents.size(); i++)
    {
        id = sIndexAndLocalIDToUUID[mOrphanParents[i]];
        LLViewerObject *objectp = findObject(id);
        if (objectp)
        {
            std::string id_str;
            objectp->mID.toString(id_str);
            std::string tmpstr = std::string("Par:  ") + id_str;
            addDebugBeacon(objectp->getPositionAgent(),
                            tmpstr,
                            LLColor4(1.f,0.f,0.f,1.f),
                            LLColor4(1.f,1.f,1.f,1.f));
        }
    }

    LLColor4 text_color;
    for (i = 0; i < mOrphanChildren.size(); i++)
    {
        OrphanInfo oi = mOrphanChildren[i];
        LLViewerObject *objectp = findObject(oi.mChildInfo);
        if (objectp)
        {
            std::string id_str;
            objectp->mID.toString(id_str);
            std::string tmpstr;
            if (objectp->getParent())
            {
                tmpstr = std::string("ChP:  ") + id_str;
                text_color = LLColor4(0.f, 1.f, 0.f, 1.f);
            }
            else
            {
                tmpstr = std::string("ChNoP:    ") + id_str;
                text_color = LLColor4(1.f, 0.f, 0.f, 1.f);
            }
            id = sIndexAndLocalIDToUUID[oi.mParentInfo];
            addDebugBeacon(objectp->getPositionAgent() + LLVector3(0.f, 0.f, -0.25f),
                            tmpstr,
                            LLColor4(0.25f,0.25f,0.25f,1.f),
                            text_color);
        }
        i++;
    }
    */

    sample(LLStatViewer::NUM_OBJECTS, mObjects.size());
    sample(LLStatViewer::NUM_ACTIVE_OBJECTS, idle_count);
}

// GetObjectCost: ids per POST, and how many ids may be in flight before the next
// batch waits. Large Area Search result sets would otherwise go out as one huge POST
// (the service can reject oversized requests) or as a burst of concurrent POSTs.
// The GetObjectCost service refuses more than 500 ids per request
// ("ObjectCost_TooManyObjects: Maximum number of resource cost requests allowed is 500").
constexpr size_t MAX_OBJECT_COST_REQUEST = 500;
constexpr size_t MAX_PENDING_OBJECT_COST = 4 * MAX_OBJECT_COST_REQUEST;

void LLViewerObjectList::fetchObjectCosts()
{
    // Each object's cost comes from ITS region's GetObjectCost service: a region only costs
    // its own objects, so asking the agent's region for a neighbour's objects left them at 0.
    // Batches are built here (main thread) per region, at most the service's 500 ids each.
    static constexpr F64 OBJECT_COST_MIN_INTERVAL = 2.0;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= mObjectCostAskedPrune)
    {
        mObjectCostAskedPrune = now + 1.0;
        for (auto it = mObjectCostAskedAt.begin(); it != mObjectCostAskedAt.end(); )
        {
            it = (now - it->second >= OBJECT_COST_MIN_INTERVAL) ? mObjectCostAskedAt.erase(it) : std::next(it);
        }
    }
    // Re-queue costs that failed to arrive and whose backoff has elapsed (selected objects only)
    for (auto it = mObjectCostRetry.begin(); it != mObjectCostRetry.end(); )
    {
        if (it->second.mNext > now)
        {
            ++it;
            continue;
        }
        LLViewerObject* objectp = findObject(it->first);
        LLViewerObject* rootp = objectp ? objectp->getRootEdit() : nullptr;
        if (objectp && objectp->isCostStale() && !objectp->isLocalOnly() && (objectp->isSelected() || (rootp && rootp->isSelected())))
        {
            mStaleObjectCost.insert(it->first);
            it->second.mNext = std::numeric_limits<F64>::max();   // until it fails again (rescheduled) or succeeds (erased)
            ++it;
        }
        else
        {
            it = mObjectCostRetry.erase(it);   // gone, answered, or no longer selected
        }
    }
    while (!mStaleObjectCost.empty() && mPendingObjectCost.size() < MAX_PENDING_OBJECT_COST)
    {
        LLViewerRegion* regionp = nullptr;
        std::string url;
        LLSD idList(LLSD::emptyArray());
        for (auto it = mStaleObjectCost.begin(); it != mStaleObjectCost.end(); )
        {
            const LLUUID id = *it;
            if (mPendingObjectCost.count(id))
            {
                it = mStaleObjectCost.erase(it);   // already being asked
                continue;
            }
            auto asked = mObjectCostAskedAt.find(id);
            if (asked != mObjectCostAskedAt.end() && now - asked->second < OBJECT_COST_MIN_INTERVAL)
            {
                ++it;   // asked moments ago: stays queued, asked again once the interval passes
                continue;
            }
            LLViewerObject* objectp = findObject(id);
            LLViewerRegion* object_region = objectp ? objectp->getRegion() : nullptr;
            if (!object_region)
            {
                scheduleObjectCostRetry(id);
                it = mStaleObjectCost.erase(it);   // gone, or not in any region
                continue;
            }
            if (!regionp)
            {
                url = object_region->getCapability("GetObjectCost");
                if (url.empty())
                {
                    scheduleObjectCostRetry(id);
                    it = mStaleObjectCost.erase(it);   // region without the service (or caps not in yet)
                    continue;
                }
                regionp = object_region;
            }
            if (object_region != regionp)
            {
                ++it;   // another region: a later batch
                continue;
            }
            idList.append(id);
            mPendingObjectCost.insert(id);
            mObjectCostAskedAt[id] = now;
            it = mStaleObjectCost.erase(it);
            if (idList.size() >= (S32)MAX_OBJECT_COST_REQUEST)
            {
                break;
            }
        }
        if (!regionp || idList.size() == 0)
        {
            break;
        }
        LLCoros::instance().launch("LLViewerObjectList::fetchObjectCostsCoro",
            boost::bind(&LLViewerObjectList::fetchObjectCostsCoro, this, url, idList, regionp->getName()));
    }
}

/*static*/
void LLViewerObjectList::reportObjectCostFailure(LLSD &objectList)
{
    // TODO*: No more hard coding
    for (LLSD::array_iterator it = objectList.beginArray(); it != objectList.endArray(); ++it)
    {
        gObjectList.onObjectCostFetchFailure(it->asUUID());
    }
}


void LLViewerObjectList::fetchObjectCostsCoro(std::string url, LLSD idList, std::string region_name)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
        httpAdapter(new LLCoreHttpUtil::HttpCoroutineAdapter("genericPostCoro", httpPolicy));
    LLCore::HttpRequest::ptr_t httpRequest(new LLCore::HttpRequest);
    const S32 batched = idList.size();

    LLSD postData = LLSD::emptyMap();

    postData["object_ids"] = idList;

    LLTimer cost_timer;
    LLSD result = httpAdapter->postAndSuspend(httpRequest, url, postData);
    // One line per batch: how long the cost service takes and how much is still queued.
    LL_INFOS("ObjectCost") << "GetObjectCost (" << region_name << "): " << batched << " ids in " << cost_timer.getElapsedTimeF32()
                           << "; still queued " << mStaleObjectCost.size() << ", in flight " << mPendingObjectCost.size() << LL_ENDL;

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    if (!status || result.has("error"))
    {
        if (result.has("error"))
        {
            LL_WARNS() << "Application level error when fetching object "
                << "cost.  Message: " << result["error"]["message"].asString()
                << ", identifier: " << result["error"]["identifier"].asString()
                << LL_ENDL;

            // Refused as too many: put the ids back so the next batches (already capped at
            // the service limit) ask again, instead of leaving their cost at 0.
            if (result["error"]["identifier"].asString() == "ObjectCost_TooManyObjects")
            {
                for (LLSD::array_iterator it = idList.beginArray(); it != idList.endArray(); ++it)
                {
                    mPendingObjectCost.erase(it->asUUID());
                    mStaleObjectCost.insert(it->asUUID());
                }
                return;
            }
        }
        reportObjectCostFailure(idList);

        return;
    }

    // Success, grab the resource cost and linked set costs
    // for an object if one was returned
    for (LLSD::array_iterator it = idList.beginArray(); it != idList.endArray(); ++it)
    {
        LLUUID objectId = it->asUUID();

        // Object could have been added to the mStaleObjectCost after request started
        mStaleObjectCost.erase(objectId);
        mPendingObjectCost.erase(objectId);

        // Check to see if the request contains data for the object
        if (result.has(it->asString()))
        {
            LLSD objectData = result[it->asString()];

            F32 linkCost = (F32)objectData["linked_set_resource_cost"].asReal();
            F32 objectCost = (F32)objectData["resource_cost"].asReal();
            F32 physicsCost = (F32)objectData["physics_cost"].asReal();
            F32 linkPhysicsCost = (F32)objectData["linked_set_physics_cost"].asReal();

            gObjectList.updateObjectCost(objectId, objectCost, linkCost, physicsCost, linkPhysicsCost);
        }
        else
        {
            // TODO*: Give user feedback about the missing data?
            gObjectList.onObjectCostFetchFailure(objectId);
        }
    }

}

void LLViewerObjectList::fetchPhysicsFlags()
{
    // As for costs: each object's physics data comes from its own region's service, in
    // batches built here per region (a region only answers for its own objects). At most
    // 4 batches per frame; the rest stay stale for the next frames.
    for (S32 launched = 0; launched < 4 && !mStalePhysicsFlags.empty(); ++launched)
    {
        LLViewerRegion* regionp = nullptr;
        std::string url;
        LLSD idList(LLSD::emptyArray());
        for (auto it = mStalePhysicsFlags.begin(); it != mStalePhysicsFlags.end(); )
        {
            const LLUUID id = *it;
            if (mPendingPhysicsFlags.count(id))
            {
                it = mStalePhysicsFlags.erase(it);
                continue;
            }
            LLViewerObject* objectp = findObject(id);
            LLViewerRegion* object_region = objectp ? objectp->getRegion() : nullptr;
            if (!object_region)
            {
                it = mStalePhysicsFlags.erase(it);
                continue;
            }
            if (!regionp)
            {
                url = object_region->getCapability("GetObjectPhysicsData");
                if (url.empty())
                {
                    it = mStalePhysicsFlags.erase(it);
                    continue;
                }
                regionp = object_region;
            }
            if (object_region != regionp)
            {
                ++it;
                continue;
            }
            idList.append(id);
            mPendingPhysicsFlags.insert(id);
            it = mStalePhysicsFlags.erase(it);
            if (idList.size() >= (S32)MAX_CONCURRENT_PHYSICS_REQUESTS)
            {
                break;
            }
        }
        if (!regionp || idList.size() == 0)
        {
            break;
        }
        LLCoros::instance().launch("LLViewerObjectList::fetchPhisicsFlagsCoro",
            boost::bind(&LLViewerObjectList::fetchPhisicsFlagsCoro, this, url, idList));
    }
}

/*static*/
void LLViewerObjectList::reportPhysicsFlagFailure(LLSD &objectList)
{
    // TODO*: No more hard coding
    for (LLSD::array_iterator it = objectList.beginArray(); it != objectList.endArray(); ++it)
    {
        gObjectList.onPhysicsFlagsFetchFailure(it->asUUID());
    }
}

void LLViewerObjectList::fetchPhisicsFlagsCoro(std::string url, LLSD idList)
{
    LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t
        httpAdapter(new LLCoreHttpUtil::HttpCoroutineAdapter("genericPostCoro", httpPolicy));
    LLCore::HttpRequest::ptr_t httpRequest(new LLCore::HttpRequest);

    LLSD postData = LLSD::emptyMap();

    postData["object_ids"] = idList;

    LLSD result = httpAdapter->postAndSuspend(httpRequest, url, postData);

    LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);

    if (!status || result.has("error"))
    {
        if (result.has("error"))
        {
            LL_WARNS() << "Application level error when fetching object "
                << "physics flags.  Message: " << result["error"]["message"].asString()
                << ", identifier: " << result["error"]["identifier"].asString()
                << LL_ENDL;

            // TODO*: Adaptively adjust request size if the
            // service says we've requested too many and retry
        }
        reportPhysicsFlagFailure(idList);

        return;
    }

    // Success, grab the resource cost and linked set costs
    // for an object if one was returned
    for (LLSD::array_iterator it = idList.beginArray(); it != idList.endArray(); ++it)
    {
        LLUUID objectId = it->asUUID();

        // Check to see if the request contains data for the object
        if (result.has(it->asString()))
        {
            const LLSD& data = result[it->asString()];

            S32 shapeType = data["PhysicsShapeType"].asInteger();

            gObjectList.updatePhysicsShapeType(objectId, shapeType);

            if (data.has("Density"))
            {
                F32 density = (F32)data["Density"].asReal();
                F32 friction = (F32)data["Friction"].asReal();
                F32 restitution = (F32)data["Restitution"].asReal();
                F32 gravityMult = (F32)data["GravityMultiplier"].asReal();

                gObjectList.updatePhysicsProperties(objectId, density,
                    friction, restitution, gravityMult);
            }
        }
        else
        {
            // TODO*: Give user feedback about the missing data?
            gObjectList.onPhysicsFlagsFetchFailure(objectId);
        }
    }
}

void LLViewerObjectList::clearDebugText()
{
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        (*iter)->restoreHudText();
    }
}


void LLViewerObjectList::cleanupReferences(LLViewerObject *objectp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    bool new_dead_object = true;
    if (mDeadObjects.find(objectp->mID) != mDeadObjects.end())
    {
        LL_INFOS() << "Object " << objectp->mID << " already on dead list!" << LL_ENDL;
        new_dead_object = false;
    }
    else
    {
        mDeadObjects.insert(objectp->mID);
    }

    // Cleanup any references we have to this object
    // Remove from object map so noone can look it up.

    LL_DEBUGS("ObjectUpdate") << " dereferencing id " << objectp->mID << LL_ENDL;

    mUUIDObjectMap.erase(objectp->mID);

    //if (objectp->getRegion())
    //{
    //  LL_INFOS() << "cleanupReferences removing object from table, local ID " << objectp->mLocalID << ", ip "
    //              << objectp->getRegion()->getHost().getAddress() << ":"
    //              << objectp->getRegion()->getHost().getPort() << LL_ENDL;
    //}

    removeFromLocalIDTable(objectp);

    if (objectp->onActiveList())
    {
        //LL_INFOS() << "Removing " << objectp->mID << " " << objectp->getPCodeString() << " from active list in cleanupReferences." << LL_ENDL;
        objectp->setOnActiveList(false);
        removeFromActiveList(objectp);
    }

    if (objectp->isOnMap())
    {
        removeFromMap(objectp);
    }

    // Don't clean up mObject references, these will be cleaned up more efficiently later!

    if(new_dead_object)
    {
        mNumDeadObjects++;
    }
}

bool LLViewerObjectList::killObject(LLViewerObject *objectp)
{
    LL_PROFILE_ZONE_SCOPED;
    // Don't ever kill gAgentAvatarp, just force it to the agent's region
    // unless region is NULL which is assumed to mean you are logging out.
    if ((objectp == gAgentAvatarp) && gAgent.getRegion())
    {
        objectp->setRegion(gAgent.getRegion());
        return false;
    }

    // When we're killing objects, all we do is mark them as dead.
    // We clean up the dead objects later.

    if (objectp)
    {
        // We are going to cleanup a lot of smart pointers to this object, they might be last,
        // and object being NULLed while inside it's own function won't be pretty
        // so create a pointer to make sure object will stay alive untill markDead() finishes
        LLPointer<LLViewerObject> sp(objectp);
        sp->markDead(); // does the right thing if object already dead
        return true;
    }

    return false;
}

// #607: our avatar's region follows the agent only when the new sim's update for it
// re-keys it (or killObject rescues it). A region dropped before that (teleport, ring drop
// right after a crossing) would kill our attachments, which ride on the avatar's region:
// move the avatar -- and with it every attachment (setRegion cascades to children) -- to
// the agent region first, as killObject would for the avatar alone.
static void rehome_self_from(LLViewerRegion* regionp)
{
    LLViewerRegion* agent_region = gAgent.getRegion();
    if (regionp && agent_region && agent_region != regionp && isAgentAvatarValid()
        && gAgentAvatarp->getRegion() == regionp)
    {
        gAgentAvatarp->setRegion(agent_region);
        LL_INFOS("CrossingKeep") << "region " << regionp->getName()
                                 << " going away before our avatar was re-homed: moved it and its attachments to "
                                 << agent_region->getName() << LL_ENDL;
    }
}

void LLViewerObjectList::killObjects(LLViewerRegion *regionp)
{
    LL_PROFILE_ZONE_SCOPED;
    rehome_self_from(regionp);

    // Release our client-only local mesh previews hosted by this region before it
    // is torn down, so they don't dangle past it (the preview avatar too).
    if (LLLocalMeshMgr::instanceExists())
    {
        LLLocalMeshMgr::getInstance()->despawnObjectsInRegion(regionp);
    }

    // #202: local ids of derendered (never created) objects in this region become meaningless
    if ( (regionp) && (LLDerenderList::instanceExists()) )
    {
        LLDerenderList::instance().onRegionRemoved(regionp->getHandle());
    }

    LLViewerObject *objectp;

    U32 killed = 0;
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        objectp = *iter;

        if (objectp->mRegionp == regionp)
        {
            // The region is going away; nothing will animate this object again.
            LLObjectSignaledAnimationMap::instance().erase(objectp->getID());
            ++killed;   // (dead ones too: they leave the list below, before the region goes)
            killObject(objectp);
        }
    }

    // Have to clean right away because the region is becoming invalid. (#568: only when
    // this region still had objects listed -- after an incremental teardown they are dead
    // and already compacted, and the full list scan would only be for other regions'.)
    // The compaction is immediate; the destructors of THIS region's objects run below,
    // other regions' stay on the per-frame budget.
    if (killed)
    {
        cleanDeadObjects(true);
    }
    // Every deferred destructor of this region's objects, before the region goes.
    drainPendingDestroy(0.f, regionp);
}

void LLViewerObjectList::collectRegionObjects(LLViewerRegion* regionp, std::vector<LLUUID>& out)
{
    LL_PROFILE_ZONE_SCOPED;
    if (LLLocalMeshMgr::instanceExists())
    {
        LLLocalMeshMgr::getInstance()->despawnObjectsInRegion(regionp);
    }
    rehome_self_from(regionp);   // #607
    out.clear();
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;
        if (objectp && objectp->mRegionp == regionp && !objectp->isDead())
        {
            out.push_back(objectp->getID());
        }
    }
}

bool LLViewerObjectList::killRegionObjectsSliced(LLViewerRegion* regionp, const std::vector<LLUUID>& objects,
                                                 size_t& cursor, F64 budget_s, U32& killed)
{
    LL_PROFILE_ZONE_SCOPED;
    LLTimer timer;
    U32 n = 0;
    rehome_self_from(regionp);   // #607
    while (cursor < objects.size())
    {
        if (n > 0 && timer.getElapsedTimeF64().value() > budget_s)
        {
            break;
        }
        // Still alive (a dead object left the UUID map) and still the region's (an update
        // may have moved it to a listed region meanwhile, or a parent's death killed it).
        LLViewerObject* objectp = findObject(objects[cursor++]);
        if (objectp && !objectp->isDead() && objectp->mRegionp == regionp)
        {
            LLObjectSignaledAnimationMap::instance().erase(objectp->getID());
            killObject(objectp);
            ++n;
        }
    }
    killed += n;
    return cursor >= objects.size();
}

void LLViewerObjectList::killAllObjects()
{
    // Used only on global destruction.

    // Destroy local-mesh preview objects (and their preview avatar) first, so
    // these client-only objects are released while this list is still valid
    // instead of dangling until the local-mesh singleton is torn down.
    if (LLLocalMeshMgr::instanceExists())
    {
        LLLocalMeshMgr::getInstance()->cleanup();
    }

    // Mass cleanup to not clear lists one item at a time
    mIndexAndLocalIDToUUID.clear();
    mActiveObjects.clear();
    mMapObjects.clear();

    LLViewerObject *objectp;
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        objectp = *iter;
        objectp->setOnActiveList(false);
        objectp->setListIndex(-1);
        objectp->mRegionIndex = 0;
        objectp->mOnMap = false;
        objectp->setMapListIndex(-1);
        killObject(objectp);
        // Object must be dead, or it's the LLVOAvatarSelf which never dies.
        llassert((objectp == gAgentAvatarp) || objectp->isDead());
    }

    cleanDeadObjects(false);

    if(!mObjects.empty())
    {
        LL_WARNS() << "LLViewerObjectList::killAllObjects still has entries in mObjects: " << mObjects.size() << LL_ENDL;
        mObjects.clear();
    }
}

void LLViewerObjectList::cleanDeadObjects(bool use_timer)
{
    if (mNumDeadObjects)
    {
        LL_PROFILE_ZONE_SCOPED;
        const F64 now = LLFrameTimer::getElapsedSeconds();
        S32 num_removed = 0;
        LLViewerObject *objectp;

        vobj_list_t::reverse_iterator target = mObjects.rbegin();

        vobj_list_t::iterator iter = mObjects.begin();
        for ( ; iter != mObjects.end(); )
        {
            // Scan for all of the dead objects and put them all on the end of the list with no ref count ops
            objectp = *iter;
            if (objectp == NULL)
            { //we caught up to the dead tail
                break;
            }

            if (objectp->isDead())
            {
                LLPointer<LLViewerObject>::swap(*iter, *target);
                // The list's reference moves to the destruction backlog (*target becomes
                // NULL, as before); the destructor runs from drainPendingDestroy.
                mPendingDestroy.emplace_back();
                LLPointer<LLViewerObject>::swap(mPendingDestroy.back().mObject, *target);
                mPendingDestroy.back().mQueuedAt = now;
                LLViewerObject* deadp = mPendingDestroy.back().mObject.get();
                if (deadp && deadp->isAvatar())
                {
                    // Avatar walks (LLCharacter::sInstances) must not see a dead avatar while
                    // it waits for its destructor (which removes it again: a no-op then).
                    LLCharacter::sInstances.remove(static_cast<LLCharacter*>(static_cast<LLVOAvatar*>(deadp)));
                }
                ++mDestroyStats.mDeferred;
                ++target;
                num_removed++;

                if (num_removed == mNumDeadObjects || iter->isNull())
                {
                    // We've cleaned up all of the dead objects or caught up to the dead tail
                    break;
                }
            }
            else
            {
                ++iter;
            }
        }

        llassert(num_removed == mNumDeadObjects);

        //erase as a block
        mObjects.erase(mObjects.begin()+(mObjects.size()-mNumDeadObjects), mObjects.end());

        // We've cleaned the global object list, now let's do some paranoia testing on objects
        // before blowing away the dead list.
        mDeadObjects.clear();
        mNumDeadObjects = 0;
        mDestroyStats.mPeak = llmax(mDestroyStats.mPeak, (U32)mPendingDestroy.size());
    }

    if (!use_timer)
    {
        drainPendingDestroy(0.f);
        return;
    }
    // Per-frame budget (at least one destructor per frame). A backlog past 4096 objects
    // or older than 2 s (a whole region's teardown) drains 4x faster.
    static LLCachedControl<F32> s_budget_ms(gSavedSettings, "SLObjectDestroyBudgetMs", 1.5f);
    F32 budget_ms = s_budget_ms;
    if (budget_ms <= 0.f)
    {
        drainPendingDestroy(0.f);   // 0 = LL's synchronous destruction
    }
    else if (!mPendingDestroy.empty())
    {
        const F64 oldest = LLFrameTimer::getElapsedSeconds() - mPendingDestroy.front().mQueuedAt;
        if (mPendingDestroy.size() > 4096 || oldest > 2.0)
        {
            budget_ms *= 4.f;
        }
        drainPendingDestroy(budget_ms);
    }
    logDestroyStats(LLFrameTimer::getElapsedSeconds());
}

void LLViewerObjectList::destroyPendingAt(size_t idx)
{
    LLPointer<LLViewerObject> obj;
    LLPointer<LLViewerObject>::swap(obj, mPendingDestroy[idx].mObject);
    if (obj.isNull() || obj->getNumRefs() > 1)
    {
        return;   // someone else still holds it: its destructor runs when they let go
    }
    // The last reference: time the destructor (the stat line names the slowest).
    const LLPCode pcode = obj->getPCode();
    const bool is_avatar = obj->isAvatar();
    const bool is_control = is_avatar && static_cast<LLVOAvatar*>(obj.get())->isControlAvatar();
    const bool is_attachment = obj->isAttachment();
    const S32 num_tes = (S32)obj->getNumTEs();
    LLTimer timer;
    obj = NULL;
    const F32 ms = timer.getElapsedTimeF32().value() * 1000.f;
    ++mDestroyStats.mDestroyed;
    if (ms > mDestroyStats.mWorstMs)
    {
        mDestroyStats.mWorstMs = ms;
        mDestroyStats.mWorstWhat = llformat("pcode %d%s%s, %d faces", (S32)pcode,
                                            is_control ? " animesh avatar" : (is_avatar ? " avatar" : ""),
                                            is_attachment ? " attachment" : "", num_tes);
    }
}

void LLViewerObjectList::drainPendingDestroy(F32 budget_ms, LLViewerRegion* regionp)
{
    if (mPendingDestroy.empty())
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED;
    VK_HITCH_SCOPE("LL_destroyObjects");
    LLTimer timer;
    U32 n = 0;
    if (regionp)
    {
        // A region about to be deleted: every pending object of it goes now (a destructor
        // must not outlive the region its object points at); the rest keep their order.
        std::deque<PendingDestroy> keep;
        for (size_t i = 0; i < mPendingDestroy.size(); ++i)
        {
            LLViewerObject* objp = mPendingDestroy[i].mObject.get();
            if (!objp)
            {
                continue;
            }
            if (objp->getRegion() == regionp)
            {
                destroyPendingAt(i);
                ++n;
            }
            else
            {
                keep.push_back(std::move(mPendingDestroy[i]));
            }
        }
        mPendingDestroy.swap(keep);
    }
    else
    {
        const F64 budget_s = budget_ms * 0.001;
        while (!mPendingDestroy.empty())
        {
            if (budget_ms > 0.f && n > 0 && timer.getElapsedTimeF64().value() > budget_s)
            {
                break;
            }
            destroyPendingAt(0);
            mPendingDestroy.pop_front();
            ++n;
        }
    }
    mDestroyStats.mMaxDrainMs = llmax(mDestroyStats.mMaxDrainMs, timer.getElapsedTimeF32().value() * 1000.f);
    LLVKHitch::count("objDestroyed", n);
    LLVKHitch::count("objDestroyBacklog", (U32)mPendingDestroy.size());
}

void LLViewerObjectList::logDestroyStats(F64 now)
{
    DestroyStats& st = mDestroyStats;
    if (now - st.mLastLog < 5.0)
    {
        return;
    }
    if (st.mDestroyed || st.mDeferred || !mPendingDestroy.empty())
    {
        LL_INFOS("ObjDestroy") << "object destruction (" << (S32)(now - st.mLastLog) << " s): " << st.mDeferred
                               << " queued, " << st.mDestroyed << " destroyed, backlog " << mPendingDestroy.size()
                               << " (peak " << st.mPeak << "), slowest frame " << st.mMaxDrainMs
                               << " ms, slowest destructor " << st.mWorstMs << " ms (" << st.mWorstWhat << ")" << LL_ENDL;
    }
    const U32 backlog = (U32)mPendingDestroy.size();
    st = DestroyStats{};
    st.mPeak = backlog;
    st.mLastLog = now;
}

void LLViewerObjectList::removeFromActiveList(LLViewerObject* objectp)
{
    S32 idx = objectp->getListIndex();
    if (idx != -1)
    {
        objectp->setListIndex(-1);

        S32 size = (S32)mActiveObjects.size();
        if (size > 0) // mActiveObjects could have been cleaned already
        {
            // Remove by moving last element to this object's position

            llassert(idx < size); // idx should be always within mActiveObjects, unless killAllObjects was called
            llassert(mActiveObjects[idx] == objectp); // object should be there

            S32 last_index = size - 1;
            if (idx < last_index)
            {
                mActiveObjects[idx] = mActiveObjects[last_index];
                mActiveObjects[idx]->setListIndex(idx);
            } // else assume it's the last element, no need to swap
            mActiveObjects.pop_back();
        }
    }
}

void LLViewerObjectList::updateActive(LLViewerObject *objectp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    if (objectp->isDead())
    {
        return; // We don't update dead objects!
    }

    bool active = objectp->isActive();
    if (active != objectp->onActiveList())
    {
        if (active)
        {
            //LL_INFOS() << "Adding " << objectp->mID << " " << objectp->getPCodeString() << " to active list." << LL_ENDL;
            S32 idx = objectp->getListIndex();
            if (idx <= -1)
            {
                mActiveObjects.push_back(objectp);
                objectp->setListIndex(static_cast<S32>(mActiveObjects.size()) - 1);
            objectp->setOnActiveList(true);
            }
            else
            {
                llassert(idx < mActiveObjects.size());
                llassert(mActiveObjects[idx] == objectp);

                if (idx >= mActiveObjects.size() ||
                    mActiveObjects[idx] != objectp)
                {
                    LL_WARNS() << "Invalid object list index detected!" << LL_ENDL;
                }
            }
        }
        else
        {
            //LL_INFOS() << "Removing " << objectp->mID << " " << objectp->getPCodeString() << " from active list." << LL_ENDL;
            removeFromActiveList(objectp);
            objectp->setOnActiveList(false);
        }
    }

    //post condition: if object is active, it must be on the active list
    llassert(!active || std::find(mActiveObjects.begin(), mActiveObjects.end(), objectp) != mActiveObjects.end());

    //post condition: if object is not active, it must not be on the active list
    llassert(active || std::find(mActiveObjects.begin(), mActiveObjects.end(), objectp) == mActiveObjects.end());
}

void LLViewerObjectList::updateObjectCost(LLViewerObject* object)
{
    // Client-only (local preview) objects have no sim counterpart, so their fake
    // UUIDs can't be costed by the GetObjectCost capability: a request would never
    // resolve and would re-fire every time the cost is read. Skip them -- their
    // cached cost stays at the 0 default, which is the correct land impact for
    // something that isn't actually rezzed on the region.
    if (!object || object->isLocalOnly())
    {
        return;
    }
    if (!object->isRoot())
    { //always fetch cost for the parent when fetching cost for children
        mStaleObjectCost.insert(((LLViewerObject*)object->getParent())->getID());
    }
    mStaleObjectCost.insert(object->getID());
}

void LLViewerObjectList::updateObjectCost(const LLUUID& object_id, F32 object_cost, F32 link_cost, F32 physics_cost, F32 link_physics_cost)
{
    mObjectCostRetry.erase(object_id);
    LLViewerObject* object = findObject(object_id);
    if (object)
    {
        object->setObjectCost(object_cost);
        object->setLinksetCost(link_cost);
        object->setPhysicsCost(physics_cost);
        object->setLinksetPhysicsCost(link_physics_cost);
    }

    // Area Search builds its Land Impact cells before most costs are known; fill them in
    // as the replies arrive instead of waiting for a list refresh.
    if (FSAreaSearch* area_search = LLFloaterReg::findTypedInstance<FSAreaSearch>("area_search");
        area_search && area_search->isActive())
    {
        area_search->updateObjectCosts(object_id, object_cost, link_cost, physics_cost, link_physics_cost);
    }
}

void LLViewerObjectList::onObjectCostFetchFailure(const LLUUID& object_id)
{
    //LL_WARNS() << "Failed to fetch object cost for object: " << object_id << LL_ENDL;
    mPendingObjectCost.erase(object_id);
    scheduleObjectCostRetry(object_id);
}

void LLViewerObjectList::scheduleObjectCostRetry(const LLUUID& object_id)
{
    // 2, 4, 8, 16, 30, 30 ... seconds. fetchObjectCosts() drops the entry once the object is
    // gone, answered or deselected, so a permanently failing object can't keep asking.
    ObjectCostRetry& retry = mObjectCostRetry[object_id];
    retry.mAttempts = llmin(retry.mAttempts + 1, 8);
    retry.mNext = LLFrameTimer::getElapsedSeconds() + llmin(30.0, 2.0 * (F64)(1 << (retry.mAttempts - 1)));
}

void LLViewerObjectList::updatePhysicsFlags(const LLViewerObject* object)
{
    // Client-only (local preview) objects have no sim counterpart, so the
    // GetObjectPhysicsData cap can't resolve their fake UUIDs -- skip them, mirroring
    // updateObjectCost() above.
    if (!object || object->isLocalOnly())
    {
        return;
    }
    mStalePhysicsFlags.insert(object->getID());
}

void LLViewerObjectList::updatePhysicsShapeType(const LLUUID& object_id, S32 type)
{
    mPendingPhysicsFlags.erase(object_id);
    LLViewerObject* object = findObject(object_id);
    if (object)
    {
        object->setPhysicsShapeType(type);
    }
}

void LLViewerObjectList::updatePhysicsProperties(const LLUUID& object_id,
                                                F32 density,
                                                F32 friction,
                                                F32 restitution,
                                                F32 gravity_multiplier)
{
    mPendingPhysicsFlags.erase(object_id);

    LLViewerObject* object = findObject(object_id);
    if (object)
    {
        object->setPhysicsDensity(density);
        object->setPhysicsFriction(friction);
        object->setPhysicsGravity(gravity_multiplier);
        object->setPhysicsRestitution(restitution);
    }
}

void LLViewerObjectList::onPhysicsFlagsFetchFailure(const LLUUID& object_id)
{
    //LL_WARNS() << "Failed to fetch physics flags for object: " << object_id << LL_ENDL;
    mPendingPhysicsFlags.erase(object_id);
}

void LLViewerObjectList::shiftObjects(const LLVector3 &offset)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;
    CROSSING_SCOPE(LLCrossingTrace::CH_OBJLIST_SHIFT, "Crossing shiftObjects");
    crossing_scope_.setA((U32)mObjects.size());
    // This is called when we shift our origin when we cross region boundaries...
    // We need to update many object caches, I'll document this more as I dig through the code
    // cleaning things out...

    if (0 == offset.magVecSquared())
    {
        return;
    }


    LLViewerObject *objectp;
    // #568: updatePositionCaches() did a linear LLWorld::isRegionListed search per object.
    // Objects cluster by region, so remember the last region proven listed.
    const LLViewerRegion* last_listed_region = nullptr;
    LLWorld* world = LLWorld::getInstance();
    {
    LLCrossingTrace::Scope loop_scope(LLCrossingTrace::CH_OBJLIST_LOOP);   // #568: the loop alone
    U32 marked = 0;
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        objectp = *iter;
        // There could be dead objects on the object list, so don't update stuff if the object is dead.
        if (!objectp->isDead())
        {
            const LLViewerRegion* object_region = objectp->getRegion();
            if (object_region && (object_region == last_listed_region || world->isRegionListed(object_region)))
            {
                last_listed_region = object_region;
                objectp->updatePositionCachesListed();
            }

            if (objectp->mDrawable.notNull() && !objectp->mDrawable->isDead())
            {
                gPipeline.markShift(objectp->mDrawable);
                ++marked;
            }
        }
    }
    loop_scope.setA(marked);
    }

    gPipeline.shiftObjects(offset);
    LLWorld::getInstance()->invalidateHoleWaterCache();    // #568: positions moved; force next water rebuild
    // Clean (Vulkan) pipeline: its instance store keeps a stable render-world frame (#568,
    // RenderVKStableWorldFrame) and only records the shift (plus a rare re-anchor); the
    // legacy path re-bases every instance. Without either, every registered object stayed
    // at its OLD agent position after a crossing until the sweep re-registered it.
    LLCleanIngest::instance().shiftOrigin(offset);

    {
        LLCrossingTrace::Scope regions_scope(LLCrossingTrace::CH_SHIFT_REGIONS);
        LLWorld::getInstance()->shiftRegions(offset);
    }
}

void LLViewerObjectList::repartitionObjects()
{
    for (vobj_list_t::iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;
        if (!objectp->isDead())
        {
            LLDrawable* drawable = objectp->mDrawable;
            if (drawable && !drawable->isDead())
            {
                drawable->updateBinRadius();
                drawable->updateSpatialExtents();
                drawable->movePartition();
            }
        }
    }
}

//debug code
bool LLViewerObjectList::hasMapObjectInRegion(LLViewerRegion* regionp)
{
    for (vobj_list_t::iterator iter = mMapObjects.begin(); iter != mMapObjects.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;

        if(objectp->isDead() || objectp->getRegion() == regionp)
        {
            return true ;
        }
    }

    return false ;
}

//make sure the region is cleaned up.
void LLViewerObjectList::purgeOrphansForRegion(const LLHost& host)
{
    U64 ipport = (((U64)host.getAddress()) << 32) | (U64)host.getPort();
    auto idx_it = mIPAndPortToIndex.find(ipport);
    if (idx_it == mIPAndPortToIndex.end() || !idx_it->second)
    {
        return;
    }
    const U32 region_index = idx_it->second;
    for (auto it = mOrphanChildren.begin(); it != mOrphanChildren.end(); )
    {
        if ((U32)(it->first >> 32) == region_index)
        {
            mNumOrphans -= (S32)it->second.size();
            it = mOrphanChildren.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void LLViewerObjectList::clearAllMapObjectsInRegion(LLViewerRegion* regionp)
{
    purgeOrphansForRegion(regionp->getHost());
    std::set<LLViewerObject*> dead_object_list ;
    std::set<LLViewerObject*> region_object_list ;
    for (vobj_list_t::iterator iter = mMapObjects.begin(); iter != mMapObjects.end(); ++iter)
    {
        LLViewerObject* objectp = *iter;

        if(objectp->isDead())
        {
            dead_object_list.insert(objectp) ;
        }
        else if(objectp->getRegion() == regionp)
        {
            region_object_list.insert(objectp) ;
        }
    }

    if(dead_object_list.size() > 0)
    {
        LL_WARNS() << "There are " << dead_object_list.size() << " dead objects on the map!" << LL_ENDL ;

        for(std::set<LLViewerObject*>::iterator iter = dead_object_list.begin(); iter != dead_object_list.end(); ++iter)
        {
            cleanupReferences(*iter) ;
        }
    }
    if(region_object_list.size() > 0)
    {
        LL_WARNS() << "There are " << region_object_list.size() << " objects not removed from the deleted region!" << LL_ENDL ;

        for(std::set<LLViewerObject*>::iterator iter = region_object_list.begin(); iter != region_object_list.end(); ++iter)
        {
            (*iter)->markDead() ;
        }
    }
}


size_t LLViewerObjectList::renderObjectsForMap(LLNetMap &netmap, size_t start, size_t max_count)
{
    static const LLUIColor above_water_color = LLUIColorTable::instance().getColor( "NetMapOtherOwnAboveWater" );
    static const LLUIColor below_water_color = LLUIColorTable::instance().getColor( "NetMapOtherOwnBelowWater" );
    static const LLUIColor you_own_above_water_color =
                        LLUIColorTable::instance().getColor( "NetMapYouOwnAboveWater" );
    static const LLUIColor you_own_below_water_color =
                        LLUIColorTable::instance().getColor( "NetMapYouOwnBelowWater" );
    static const LLUIColor group_own_above_water_color =
                        LLUIColorTable::instance().getColor( "NetMapGroupOwnAboveWater" );
    static const LLUIColor group_own_below_water_color =
                        LLUIColorTable::instance().getColor( "NetMapGroupOwnBelowWater" );
    static const LLUIColor you_own_physical_color =
                        LLUIColorTable::instance().getColor("NetMapYouPhysical", LLColor4::red);
    static const LLUIColor group_own_physical_color =
                        LLUIColorTable::instance().getColor("NetMapGroupPhysical", LLColor4::green);
    static const LLUIColor other_own_physical_color =
                        LLUIColorTable::instance().getColor("NetMapOtherPhysical", LLColor4::green);
    static const LLUIColor scripted_object_color =
                        LLUIColorTable::instance().getColor("NetMapScripted", LLColor4::orange);
    static const LLUIColor temp_on_rez_object_color =
                        LLUIColorTable::instance().getColor("NetMapTempOnRez", LLColor4::orange);

    const F32 MIN_RADIUS_FOR_ACCENTED_OBJECTS = 2.f;

    static LLCachedControl max_radius(gSavedSettings, "MiniMapPrimMaxRadius", 16.f);
    static LLCachedControl max_zdistance_from_avatar(gSavedSettings, "MiniMapPrimMaxVertDistance", 256.f);
    static LLCachedControl netmap_scripted(gSavedSettings, "MiniMapPrimScripted", false);
    static LLCachedControl netmap_physical(gSavedSettings, "MiniMapPrimPhysical", true);
    static LLCachedControl netmap_temp_on_rez(gSavedSettings, "MiniMapPrimTempOnRez", false);
    static LLCachedControl netmap_phantom_opacity(gSavedSettings, "MiniMapPrimPhantomOpacity", 100U);

    const size_t end = std::min(mMapObjects.size(), start + std::min(max_count, mMapObjects.size()));
    const F64 agent_z = gAgent.getPositionGlobal().mdV[VZ];
    for (size_t oi = start; oi < end; ++oi)
    {
        LLViewerObject* objectp = mMapObjects[oi];
        if(objectp->isDead())//some dead objects somehow not cleaned.
        {
            continue ;
        }

        if (!objectp->getRegion() || objectp->isOrphaned() || objectp->isAttachment())
        {
            continue;
        }
        const LLVector3& scale = objectp->getScale();
        const LLVector3d pos = objectp->getPositionGlobal();
        const F64 water_height = F64( objectp->getRegion()->getWaterHeight() );

        // Skip all objects that are more than MiniMapPrimMaxVertDistance above or below the avatar
        if (max_zdistance_from_avatar > 0.0)
        {
            F64 zdistance = pos.mdV[VZ] - agent_z;
            if (zdistance < (-max_zdistance_from_avatar) || zdistance > max_zdistance_from_avatar)
            {
                continue;
            }
        }

        F32 approx_radius = (scale.mV[VX] + scale.mV[VY]) * 0.5f * 0.5f * 1.3f;  // 1.3 is a fudge

        // Limit the size of megaprims so they don't blot out everything on the minimap.
        // Attempting to draw very large megaprims also causes client lag.
        // See DEV-17370 and DEV-29869/SNOW-79 for details.
        approx_radius = llmin(approx_radius, (F32)max_radius());

        LLColor4U color = above_water_color.get();
        if( objectp->permYouOwner() )
        {
            const F32 MIN_RADIUS_FOR_OWNED_OBJECTS = 2.f;
            if( approx_radius < MIN_RADIUS_FOR_OWNED_OBJECTS )
            {
                approx_radius = MIN_RADIUS_FOR_OWNED_OBJECTS;
            }

            if( pos.mdV[VZ] >= water_height )
            {
                if ( objectp->permGroupOwner() )
                {
                    color = group_own_above_water_color.get();
                }
                else
                {
                    color = you_own_above_water_color.get();
                }
            }
            else
            {
                if ( objectp->permGroupOwner() )
                {
                    color = group_own_below_water_color.get();
                }
                else
                {
                    color = you_own_below_water_color.get();
                }
            }
        }
        else if( pos.mdV[VZ] < water_height )
        {
            color = below_water_color.get();
        }

        if (netmap_scripted && objectp->flagScripted())
        {
            color = scripted_object_color.get();
            if( approx_radius < MIN_RADIUS_FOR_ACCENTED_OBJECTS )
            {
                approx_radius = MIN_RADIUS_FOR_ACCENTED_OBJECTS;
            }
        }

        if (netmap_physical && objectp->flagUsePhysics())
        {
            if (objectp->permYouOwner())
            {
                color = you_own_physical_color.get();
            }
            else if (objectp->permGroupOwner())
            {
                color = group_own_physical_color.get();
            }
            else
            {
                color = other_own_physical_color.get();
            }
            if( approx_radius < MIN_RADIUS_FOR_ACCENTED_OBJECTS )
            {
                approx_radius = MIN_RADIUS_FOR_ACCENTED_OBJECTS;
            }
        }

        if (netmap_temp_on_rez && objectp->flagTemporaryOnRez())
        {
            color = temp_on_rez_object_color.get();
            if( approx_radius < MIN_RADIUS_FOR_ACCENTED_OBJECTS )
            {
                approx_radius = MIN_RADIUS_FOR_ACCENTED_OBJECTS;
            }
        }

        if (objectp->flagPhantom())
        {
            color.setAlpha(llclampb(netmap_phantom_opacity()));
        }

        netmap.renderScaledPointGlobal(
            pos,
            color,
            approx_radius );
    }
    return end;
}

void LLViewerObjectList::renderObjectBounds(const LLVector3 &center)
{
}

void LLViewerObjectList::addDebugBeacon(const LLVector3 &pos_agent,
                                        const std::string &string,
                                        const LLColor4 &color,
                                        const LLColor4 &text_color,
                                        S32 line_width)
{
    LLDebugBeacon beacon;
    beacon.mPositionAgent = pos_agent;
    beacon.mString = string;
    beacon.mColor = color;
    beacon.mTextColor = text_color;
    beacon.mLineWidth = line_width;

    mDebugBeacons.push_back(beacon);
}

void LLViewerObjectList::addDebugBeacon(const LLViewerObject* objectp,
                                        const std::string &string,
                                        const LLColor4 &color,
                                        const LLColor4 &text_color,
                                        S32 line_width)
{
    if (!objectp || objectp->isDead())
    {
        return;
    }
    LLDebugBeacon& beacon = mDebugBeacons.emplace_back();
    beacon.mPositionAgent = objectp->getPositionAgent();   // fallback only; resolved at draw
    beacon.mObjectID = objectp->getID();
    beacon.mString = string;
    beacon.mColor = color;
    beacon.mTextColor = text_color;
    beacon.mLineWidth = line_width;
}

// static
bool LLViewerObjectList::getBeaconPositionAgent(LLViewerObject* objectp, LLVector3& pos_agent)
{
    if (!objectp || objectp->isDead())
    {
        return false;
    }
    if (objectp->isAvatar())
    {
        pos_agent = objectp->getRenderPosition();
        return true;
    }
    LLViewerObject* root = objectp->getRootEdit();
    if (root && root->isHUDAttachment())
    {
        return false;   // HUD space: no world position to put a beacon at
    }
    if (root && root->isAttachment())
    {
        // Worn attachment: the VK renderer draws it attach-point relative with the
        // live joint matrix (LLCleanInstanceStore flag 512) -- same composition here.
        LLVOAvatar* avatarp = root->getAvatar();
        LLViewerJointAttachment* attach_pt = avatarp ? avatarp->getTargetAttachmentPoint(root) : nullptr;
        if (!attach_pt)
        {
            pos_agent = avatarp ? avatarp->getRenderPosition() : objectp->getPositionAgent();
            return true;
        }
        LLVector3 rpos; LLQuaternion rrot;
        vkRenderLocalPose(root, rpos, rrot);
        LLVector3 local = rpos;
        if (objectp != root)
        {
            LLVector3 cpos; LLQuaternion crot;
            vkRenderLocalPose(objectp, cpos, crot);
            local = cpos * rrot + rpos;
        }
        pos_agent = attach_pt->getWorldPosition() + local * attach_pt->getWorldRotation();
        return true;
    }
    // In-world prim: the VK renderer's owned pose (eased while LL interpolates a
    // mover, object state once settled). getRenderPosition() reads drawable state,
    // which is not maintained for settled objects under VK.
    F32 m[16], spin_axis[4], spin_pivot[4];
    LLCleanInstanceStore::composeOwnedPose(objectp, m, spin_axis, spin_pivot);
    pos_agent.set(m[12], m[13], m[14]);
    return pos_agent.isFinite();
}

void LLViewerObjectList::resetObjectBeacons()
{
    mDebugBeacons.clear();
}

LLViewerObject *LLViewerObjectList::createObjectViewer(const LLPCode pcode, LLViewerRegion *regionp, S32 flags)
{
    LLUUID fullid;
    fullid.generate();

    LLViewerObject *objectp = LLViewerObject::createObject(fullid, pcode, regionp, flags);
    if (!objectp)
    {
//      LL_WARNS() << "Couldn't create object of type " << LLPrimitive::pCodeToString(pcode) << LL_ENDL;
        return NULL;
    }

    mUUIDObjectMap[fullid] = objectp;

    mObjects.push_back(objectp);

    updateActive(objectp);

    return objectp;
}

LLViewerObject *LLViewerObjectList::createObjectFromCache(const LLPCode pcode, LLViewerRegion *regionp, const LLUUID &uuid, const U32 local_id)
{
    llassert_always(uuid.notNull());

    LL_DEBUGS("ObjectUpdate") << "creating " << uuid << " local_id " << local_id << LL_ENDL;

    LLViewerObject *objectp = LLViewerObject::createObject(uuid, pcode, regionp);
    if (!objectp)
    {
//      LL_WARNS() << "Couldn't create object of type " << LLPrimitive::pCodeToString(pcode) << " id:" << fullid << LL_ENDL;
        return NULL;
    }

    objectp->mLocalID = local_id;
    mUUIDObjectMap[uuid] = objectp;
    setUUIDAndLocal(uuid,
                    local_id,
                    regionp->getHost().getAddress(),
                    regionp->getHost().getPort(),
                    objectp);
    mObjects.push_back(objectp);

    updateActive(objectp);
    LLCrossingKeep::onCreated(objectp);   // #607: kill -> re-create churn
    LLCrossingTrace::onObjectCreated(regionp);   // #850 first_full_frame

    return objectp;
}

LLViewerObject *LLViewerObjectList::createObject(const LLPCode pcode, LLViewerRegion *regionp,
                                                 const LLUUID &uuid, const U32 local_id, const LLHost &sender)
{
    LLUUID fullid;
    if (uuid == LLUUID::null)
    {
        fullid.generate();
    }
    else
    {
        fullid = uuid;
    }

    LL_DEBUGS("ObjectUpdate") << "createObject creating " << fullid << LL_ENDL;

    LLViewerObject *objectp = LLViewerObject::createObject(fullid, pcode, regionp);
    if (!objectp)
    {
//      LL_WARNS() << "Couldn't create object of type " << LLPrimitive::pCodeToString(pcode) << " id:" << fullid << LL_ENDL;
        return NULL;
    }
    if(regionp)
    {
        regionp->addToCreatedList(local_id);
    }

    mUUIDObjectMap[fullid] = objectp;
    setUUIDAndLocal(fullid,
                    local_id,
                    gMessageSystem->getSenderIP(),
                    gMessageSystem->getSenderPort(),
                    objectp);

    mObjects.push_back(objectp);

    updateActive(objectp);
    LLCrossingKeep::onCreated(objectp);   // #607: kill -> re-create churn
    LLCrossingTrace::onObjectCreated(regionp);   // #850 first_full_frame

    return objectp;
}

LLViewerObject *LLViewerObjectList::replaceObject(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
{
    LLViewerObject *old_instance = findObject(id);
    if (old_instance)
    {
        //cleanupReferences(old_instance);
        old_instance->markDead();

        return createObject(pcode, regionp, id, old_instance->getLocalID(), LLHost());
    }
    return NULL;
}

S32 LLViewerObjectList::findReferences(LLDrawable *drawablep) const
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWABLE;

    LLViewerObject *objectp;
    S32 num_refs = 0;

    for (vobj_list_t::const_iterator iter = mObjects.begin(); iter != mObjects.end(); ++iter)
    {
        objectp = *iter;
        if (objectp->mDrawable.notNull())
        {
            num_refs += objectp->mDrawable->findReferences(drawablep);
        }
    }
    return num_refs;
}


void LLViewerObjectList::orphanize(LLViewerObject *childp, U32 parent_id, U32 ip, U32 port)
{
    LL_DEBUGS("ORPHANS") << "Orphaning object " << childp->getID() << " with parent " << parent_id << LL_ENDL;

    // We're an orphan, flag things appropriately.
    childp->mOrphaned = true;
    if (childp->mDrawable.notNull())
    {
        bool make_invisible = true;
        LLViewerObject *parentp = (LLViewerObject *)childp->getParent();
        if (parentp)
        {
            if (parentp->getRegion() != childp->getRegion())
            {
                // This is probably an object flying across a region boundary, the
                // object probably ISN'T being reparented, but just got an object
                // update out of order (child update before parent).
                make_invisible = false;
                //LL_INFOS() << "Don't make object handoffs invisible!" << LL_ENDL;
            }
        }

        if (make_invisible)
        {
            // Make sure that this object becomes invisible if it's an orphan
            childp->mDrawable->setState(LLDrawable::FORCE_INVISIBLE);
        }
    }

    // Unknown parent, add to orpaned child list
    U64 parent_info = getIndex(parent_id, ip, port);

    std::vector<LLUUID>& children = mOrphanChildren[parent_info];
    if (std::find(children.begin(), children.end(), childp->mID) == children.end())
    {
        children.push_back(childp->mID);
        mNumOrphans++;
    }
}


void LLViewerObjectList::findOrphans(LLViewerObject* objectp, U32 ip, U32 port)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_NETWORK;

    if (objectp->isDead())
    {
        LL_WARNS() << "Trying to find orphans for dead obj " << objectp->mID
            << ":" << objectp->getPCodeString() << LL_ENDL;
        return;
    }

    //search object cache to get orphans
    if(objectp->getRegion())
    {
        objectp->getRegion()->findOrphans(objectp->getLocalID());
    }

    // See if we are a parent of an orphan.
    if (mOrphanChildren.empty())
    {
        // no known orphan parents
        return;
    }
    U64 parent_info = getIndex(objectp->mLocalID, ip, port);
    auto orphan_it = mOrphanChildren.find(parent_info);
    if (orphan_it == mOrphanChildren.end())
    {
        // did not find objectp in OrphanParent list
        return;
    }

    bool orphans_found = false;
    // Take the child list out first: re-entrant calls below may orphanize again
    std::vector<LLUUID> children = std::move(orphan_it->second);
    mOrphanChildren.erase(orphan_it);
    mNumOrphans -= (S32)children.size();

    for (const LLUUID& child_id : children)
    {
        LLViewerObject *childp = findObject(child_id);
        if (childp)
        {
            if (childp == objectp)
            {
                LL_WARNS() << objectp->mID << " has self as parent, skipping!"
                    << LL_ENDL;
                continue;
            }

            LL_DEBUGS("ORPHANS") << "Reunited parent " << objectp->mID
                << " with child " << childp->mID << LL_ENDL;
            LL_DEBUGS("ORPHANS") << "Glob: " << objectp->getPositionGlobal() << LL_ENDL;
            LL_DEBUGS("ORPHANS") << "Agent: " << objectp->getPositionAgent() << LL_ENDL;
#ifdef ORPHAN_SPAM
            addDebugBeacon(objectp->getPositionAgent(),"");
#endif
            gPipeline.markMoved(objectp->mDrawable);
            objectp->setChanged(LLXform::MOVED | LLXform::SILHOUETTE);

            // Flag the object as no longer orphaned
            childp->mOrphaned = false;
            if (childp->mDrawable.notNull())
            {
                // Make the drawable visible again and set the drawable parent
                childp->mDrawable->clearState(LLDrawable::FORCE_INVISIBLE);
                childp->setDrawableParent(objectp->mDrawable); // LLViewerObjectList::findOrphans()
                gPipeline.markRebuild( childp->mDrawable, LLDrawable::REBUILD_ALL);
            }

            // Make certain particles, icon and HUD aren't hidden
            childp->hideExtraDisplayItems( false );

            objectp->addChild(childp);
            orphans_found = true;
        }
        else
        {
            LL_INFOS() << "Missing orphan child, removing from list" << LL_ENDL;
        }
    }

    if (orphans_found && objectp->isSelected())
    {
        LLSelectNode* nodep = LLSelectMgr::getInstance()->getSelection()->findNode(objectp);
        if (nodep && !nodep->mIndividualSelection)
        {
            // rebuild selection with orphans
            LLSelectMgr::getInstance()->deselectObjectAndFamily(objectp);
            LLSelectMgr::getInstance()->selectObjectAndFamily(objectp);
        }
    }
}

////////////////////////////////////////////////////////////////////////////

LLViewerObjectList::OrphanInfo::OrphanInfo()
    : mParentInfo(0)
{
}

LLViewerObjectList::OrphanInfo::OrphanInfo(const U64 parent_info, const LLUUID child_info)
    : mParentInfo(parent_info), mChildInfo(child_info)
{
}

bool LLViewerObjectList::OrphanInfo::operator==(const OrphanInfo &rhs) const
{
    return (mParentInfo == rhs.mParentInfo) && (mChildInfo == rhs.mChildInfo);
}

bool LLViewerObjectList::OrphanInfo::operator!=(const OrphanInfo &rhs) const
{
    return !operator==(rhs);
}


LLDebugBeacon::~LLDebugBeacon() = default;
