/**
 *
 * Copyright (c) 2011-2014, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 * Slipstream (#202 / #426): avatars and other residents' attachments, item-keyed
 * attachment entries, indexed update blocking and undo for every entry type.
 * Behaviour modelled on Firestorm's derender / asset blacklist (llviewermenu.cpp
 * derenderObject(), fsassetblacklist.cpp); the code is our own.
 *
 */
#include "llviewerprecompiledheaders.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llderenderlist.h"
#include "llsdserialize.h"
#include "llselectmgr.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "lltrans.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "pipeline.h"

// ============================================================================
// LLDerenderEntry
//

LLDerenderEntry::LLDerenderEntry(EEntryType eType, bool fPersist)
    : m_eType(eType)
    , m_nFlags(fPersist ? FLAG_PERSIST : FLAG_NONE)
    , m_Date((F64)time_corrected())
{
}

LLDerenderEntry::LLDerenderEntry(const LLSD& sdData)
{
    m_eType = (sdData.has("entry_type")) ? (EEntryType)sdData["entry_type"].asInteger() : TYPE_OBJECT;
    if ( (TYPE_OBJECT != m_eType) && (TYPE_AVATAR != m_eType) && (TYPE_ATTACHMENT != m_eType) )
    {
        LL_WARNS("Derender") << "Unknown derender entry type " << (S32)m_eType << LL_ENDL;
        m_eType = TYPE_NONE;
        return;
    }

    // Pre-flags legacy support
    m_nFlags = (sdData.has("entry_flags")) ? sdData["entry_flags"].asInteger() : FLAG_PERSIST;
    // Legacy support for object_id / object_name
    m_idEntry = (sdData.has("entry_id")) ? sdData["entry_id"].asUUID() : sdData["object_id"].asUUID();
    m_strEntryName = (sdData.has("entry_name")) ? sdData["entry_name"].asString() : sdData["object_name"].asString();
    if (m_strEntryName.empty())
        m_strEntryName = LLTrans::getString("Unknown");
    if (sdData.has("entry_date"))
        m_Date = sdData["entry_date"].asDate();

    if (sdData.has("region_pos"))
        posRegion.setValue(sdData["region_pos"]);
    strRegionName = (sdData.has("region_name")) ? sdData["region_name"].asString() : LLTrans::getString("Unknown");

    m_idObject = m_idEntry;
    if (TYPE_ATTACHMENT == m_eType)
    {
        idOwner = sdData["owner_id"].asUUID();
        strAttachPt = sdData["attach_point"].asString();
        m_fItemKey = sdData["item_keyed"].asBoolean();
        // While the item stays worn its object keeps its UUID, so block that too
        if (m_fItemKey)
            m_idObject = sdData["attach_object_id"].asUUID();
    }
}

void LLDerenderEntry::setPersistent(bool fPersist)
{
    if (fPersist)
        m_nFlags |= FLAG_PERSIST;
    else
        m_nFlags &= ~FLAG_PERSIST;
}

LLSD LLDerenderEntry::toLLSD() const
{
    LLSD sdData;
    sdData["entry_type"] = m_eType;
    sdData["entry_flags"] = m_nFlags;
    sdData["entry_name"] = m_strEntryName;
    sdData["entry_id"] = m_idEntry;
    sdData["entry_date"] = m_Date;
    sdData["region_name"] = strRegionName;
    sdData["region_pos"] = posRegion.getValue();
    if (TYPE_ATTACHMENT == m_eType)
    {
        sdData["owner_id"] = idOwner;
        sdData["attach_point"] = strAttachPt;
        sdData["item_keyed"] = m_fItemKey;
        if (m_fItemKey)
            sdData["attach_object_id"] = m_idObject;
    }
    return sdData;
}

// ============================================================================
// LLDerenderList
//

LLDerenderList::change_signal_t LLDerenderList::s_ChangeSignal;
std::string LLDerenderList::s_PersistFilename = "derender_list.llsd";

LLDerenderList::LLDerenderList()
{
    load();
}

LLDerenderList::~LLDerenderList()
{
}

void LLDerenderList::load()
{
    llifstream fileDerender(gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, s_PersistFilename));
    if (!fileDerender.is_open())
    {
        LL_INFOS("Derender") << "No derender list file \"" << s_PersistFilename << "\"" << LL_ENDL;
        return;
    }

    LLSD inSD;
    S32 ret = LLSDSerialize::fromNotation(inSD, fileDerender, LLSDSerialize::SIZE_UNLIMITED);
    fileDerender.close();
    if (ret == LLSDParser::PARSE_FAILURE || !inSD.isArray())
    {
        return;
    }

    for (const LLSD& sdEntry : llsd::inArray(inSD))
    {
        auto pEntry = std::make_unique<LLDerenderEntry>(sdEntry);
        if ( (pEntry->isValid()) && (!findEntry(pEntry->getID())) )
            addEntry(std::move(pEntry));
    }
    reindexNames(); // <GMDX>
}

void LLDerenderList::save() const
{
    llofstream fileDerender(gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, s_PersistFilename));
    if (!fileDerender.is_open())
    {
        LL_WARNS("Derender") << "Can't open derender list file \"" << s_PersistFilename << "\" for writing" << LL_ENDL;
        return;
    }

    LLSD outSD = LLSD::emptyArray();
    for (const auto& pEntry : m_Entries)
    {
        if (pEntry->isPersistent())
            outSD.append(pEntry->toLLSD());
    }

    LLSDSerialize::toNotation(outSD, fileDerender);
    fileDerender.close();
}

// ============================================================================
// Entry helper functions
//

LLDerenderEntry* LLDerenderList::findEntry(const LLUUID& idEntry) const
{
    auto itEntry = std::find_if(m_Entries.begin(), m_Entries.end(), [&idEntry](const auto& e) { return idEntry == e->getID(); });
    return (m_Entries.end() != itEntry) ? itEntry->get() : nullptr;
}

const LLDerenderEntry* LLDerenderList::getEntry(const LLUUID& idEntry) const
{
    return findEntry(idEntry);
}

// <GMDX>
// static
bool LLDerenderList::hasKnownName(const LLDerenderEntry* pEntry)
{
    static const std::string s_strUnknown = LLTrans::getString("Unknown");
    return (!pEntry->m_strEntryName.empty()) && (s_strUnknown != pEntry->m_strEntryName);
}

void LLDerenderList::reindexNames()
{
    m_AttachNames.clear();
    for (const auto& pEntry : m_Entries)
    {
        if ( (LLDerenderEntry::TYPE_ATTACHMENT == pEntry->getType()) && (pEntry->isPersistent()) && (pEntry->idOwner.notNull()) && (hasKnownName(pEntry.get())) )
            m_AttachNames[pEntry->idOwner][pEntry->m_strEntryName].push_back(pEntry.get());
    }
}

// Kill a just-matched attachment once the update or reply that brought it in has been processed.
// Idle callbacks run after the network pump and before the frame is drawn, so it never shows.
static void kill_derendered_on_idle(const LLUUID& idObject)
{
    doOnIdleOneTime([idObject]()
        {
            LLViewerObject* pObj = gObjectList.findObject(idObject);
            if ( (pObj) && (!pObj->isDead()) && (LLDerenderList::instanceExists()) && (LLDerenderList::instance().isDerendered(idObject)) )
                LLDerenderList::killObject(pObj);
        });
}
// </GMDX>

void LLDerenderList::addEntry(std::unique_ptr<LLDerenderEntry> pUniqueEntry)
{
    LLDerenderEntry* pEntry = pUniqueEntry.get();
    m_Entries.push_back(std::move(pUniqueEntry));

    if (pEntry->m_idObject.notNull())
        m_BlockIds[pEntry->m_idObject] = pEntry;
    if (pEntry->isKeyedOnItem())
        m_AttachItems[pEntry->getID()] = pEntry;
}

void LLDerenderList::setBlockedObject(LLDerenderEntry* pEntry, const LLUUID& idObject)
{
    if (pEntry->m_idObject == idObject)
        return;

    auto itOld = m_BlockIds.find(pEntry->m_idObject);
    if ( (m_BlockIds.end() != itOld) && (itOld->second == pEntry) )
        m_BlockIds.erase(itOld);
    pEntry->m_idObject = idObject;
    if (idObject.notNull())
        m_BlockIds[idObject] = pEntry;
}

void LLDerenderList::setRoot(LLDerenderEntry* pEntry, U64 idRegion, U32 idRootLocal)
{
    // <GMDX> An item-keyed attachment seen in the sim: that item id is the one (see canRematch)
    if ( (idRootLocal) && (pEntry->isKeyedOnItem()) )
        pEntry->m_fItemSeen = true;
    if ( (pEntry->idRegion == idRegion) && (pEntry->idRootLocal == idRootLocal) )
        return;

    // The root moved (new region or re-rez): whatever we knew about its descendants is stale
    clearLocals(pEntry);
    pEntry->idRegion = idRegion;
    pEntry->idRootLocal = idRootLocal;
    if ( (idRegion) && (idRootLocal) )
        m_BlockLocal[local_key_t(idRegion, idRootLocal)] = pEntry;
}

void LLDerenderList::addChild(LLDerenderEntry* pEntry, U32 idLocal)
{
    if ( (!pEntry->idRegion) || (!idLocal) || (idLocal == pEntry->idRootLocal) )
        return;

    if (pEntry->idsChildLocal.end() == std::find(pEntry->idsChildLocal.begin(), pEntry->idsChildLocal.end(), idLocal))
        pEntry->idsChildLocal.push_back(idLocal);
    m_BlockLocal[local_key_t(pEntry->idRegion, idLocal)] = pEntry;
}

void LLDerenderList::clearLocals(LLDerenderEntry* pEntry)
{
    auto unindex = [this, pEntry](U32 idLocal)
    {
        auto it = m_BlockLocal.find(local_key_t(pEntry->idRegion, idLocal));
        if ( (m_BlockLocal.end() != it) && (it->second == pEntry) )
            m_BlockLocal.erase(it);
    };

    if (pEntry->idRegion)
    {
        if (pEntry->idRootLocal)
            unindex(pEntry->idRootLocal);
        for (U32 idLocal : pEntry->idsChildLocal)
            unindex(idLocal);
    }
    pEntry->idsChildLocal.clear();
    pEntry->idRootLocal = 0;
    pEntry->idRegion = 0;
}

void LLDerenderList::collectLocals(LLDerenderEntry* pEntry, const LLViewerObject* pObj)
{
    for (const LLViewerObject* pChild : pObj->getChildren())
    {
        // Avatars sitting on an object aren't part of it
        if ( (!pChild) || (pChild->isAvatar()) )
            continue;
        addChild(pEntry, pChild->getLocalID());
        collectLocals(pEntry, pChild);
    }
}

void LLDerenderList::removeEntries(const uuid_vec_t& idsEntry)
{
    bool fSave = false, fChanged = false;
    std::map<LLViewerRegion*, std::vector<U32>> idRegionObjectMap;

    for (const LLUUID& idEntry : idsEntry)
    {
        auto itEntry = std::find_if(m_Entries.begin(), m_Entries.end(), [&idEntry](const auto& e) { return idEntry == e->getID(); });
        if (m_Entries.end() == itEntry)
            continue;

        LLDerenderEntry* pEntry = itEntry->get();

        // Re-request everything we blocked (root first) so it comes back without a relog
        if (pEntry->idRegion)
        {
            if (LLViewerRegion* pRegion = LLWorld::getInstance()->getRegionFromHandle(pEntry->idRegion))
            {
                std::vector<U32>& idsLocal = idRegionObjectMap[pRegion];
                if (pEntry->idRootLocal)
                    idsLocal.push_back(pEntry->idRootLocal);
                idsLocal.insert(idsLocal.end(), pEntry->idsChildLocal.begin(), pEntry->idsChildLocal.end());
            }
        }

        clearLocals(pEntry);
        setBlockedObject(pEntry, LLUUID::null);
        if (pEntry->isKeyedOnItem())
        {
            auto itItem = m_AttachItems.find(pEntry->getID());
            if ( (m_AttachItems.end() != itItem) && (itItem->second == pEntry) )
                m_AttachItems.erase(itItem);
        }

        fSave |= pEntry->isPersistent();
        fChanged = true;
        m_Entries.erase(itEntry);
    }

    for (const auto& [pRegion, idsLocal] : idRegionObjectMap)
    {
        for (U32 idLocal : idsLocal)
            pRegion->addCacheMissFull(idLocal);
        pRegion->requestCacheMisses();
    }

    if (fChanged)
        reindexNames(); // <GMDX>
    if (fSave)
        save();
    if (fChanged)
        s_ChangeSignal();
}

void LLDerenderList::setPersistent(const uuid_vec_t& idsEntry, bool fPersist)
{
    bool fChanged = false;
    for (const LLUUID& idEntry : idsEntry)
    {
        if (LLDerenderEntry* pEntry = findEntry(idEntry))
        {
            if (pEntry->isPersistent() != fPersist)
            {
                pEntry->setPersistent(fPersist);
                fChanged = true;
            }
        }
    }
    if (fChanged)
    {
        reindexNames(); // <GMDX>
        save();
        s_ChangeSignal();
    }
}

// ============================================================================
// Object handling functions
//

// static
void LLDerenderList::killObject(LLViewerObject* pObj)
{
    if ( (!pObj) || (pObj->isDead()) )
        return;

    LLPointer<LLViewerObject> spObj(pObj);
    const LLUUID idObj = pObj->getID();

    // Display green bubble on kill [see process_kill_object()]
    if (gShowObjectUpdates)
        gPipeline.addDebugBlip(pObj->getPositionAgent(), LLColor4(0.f, 1.f, 0.f, 1.f));

    if (LLTool* pTool = LLToolMgr::getInstance()->getCurrentTool())
    {
        LLViewerObject* pEditObj = pTool->getEditingObject();
        if ( (pEditObj) && ((pEditObj == pObj) || (pEditObj->getRootEdit() == pObj)) )
            pTool->stopEditing();
    }

    // Keep the order of these the same as process_kill_object()
    LLViewerRegion* pObjRegion = pObj->getRegion(); U32 idObjLocal = pObj->getLocalID();
    const bool fCached = (!pObj->isAvatar()) && (!pObj->isAttachment());
    gObjectList.killObject(pObj);
    if ( (fCached) && (LLViewerRegion::sVOCacheCullingEnabled) && (pObjRegion) )
        pObjRegion->killCacheEntry(idObjLocal);     // also kills the child prims' entries
    LLSelectMgr::getInstance()->removeObjectFromSelections(idObj);
}

bool LLDerenderList::addObject(LLViewerObject* pObj, bool fPersist, const std::string& strName)
{
    if (!addObjectImpl(pObj, fPersist, strName))
        return false;
    if (fPersist)
        save();
    s_ChangeSignal();
    return true;
}

bool LLDerenderList::addObjectImpl(LLViewerObject* pObj, bool fPersist, const std::string& strName)
{
    if (!canAdd(pObj))
        return false;

    std::unique_ptr<LLDerenderEntry> pEntry;
    const LLViewerObject* pRegionObj = pObj;
    if (pObj->isAvatar())
    {
        pEntry = std::make_unique<LLDerenderEntry>(LLDerenderEntry::TYPE_AVATAR, fPersist);
        pEntry->m_idEntry = pObj->getID();
        pEntry->m_strEntryName = static_cast<LLVOAvatar*>(pObj)->getFullname();
    }
    else if (pObj->isAttachment())
    {
        LLVOAvatar* pAvatar = pObj->getAvatarAncestor();
        if (!pAvatar)
            return false;

        pEntry = std::make_unique<LLDerenderEntry>(LLDerenderEntry::TYPE_ATTACHMENT, fPersist);
        const LLUUID& idItem = pObj->getAttachmentItemID();
        pEntry->m_fItemKey = idItem.notNull();
        pEntry->m_idEntry = (pEntry->m_fItemKey) ? idItem : pObj->getID();
        pEntry->m_strEntryName = strName;
        pEntry->idOwner = pAvatar->getID();
        if (const LLViewerJointAttachment* pAttachPt = pAvatar->getTargetAttachmentPoint(pObj))
            pEntry->strAttachPt = pAttachPt->getName();
        pRegionObj = pAvatar;
    }
    else
    {
        pEntry = std::make_unique<LLDerenderEntry>(LLDerenderEntry::TYPE_OBJECT, fPersist);
        pEntry->m_idEntry = pObj->getID();
        pEntry->m_strEntryName = strName;
    }
    if (pEntry->m_strEntryName.empty())
        pEntry->m_strEntryName = LLTrans::getString("Unknown");
    pEntry->m_idObject = pObj->getID();

    if ( (findEntry(pEntry->getID())) || (isDerendered(pObj->getID())) )
        return false;

    if (const LLViewerRegion* pRegion = pObj->getRegion())
    {
        pEntry->strRegionName = pRegion->getName();
        pEntry->posRegion = pRegionObj->getPositionRegion();
    }
    if (pEntry->strRegionName.empty())
        pEntry->strRegionName = LLTrans::getString("Unknown");

    LLDerenderEntry* pRawEntry = pEntry.get();
    addEntry(std::move(pEntry));
    if (const LLViewerRegion* pRegion = pObj->getRegion())
    {
        setRoot(pRawEntry, pRegion->getHandle(), pObj->getLocalID());
        collectLocals(pRawEntry, pObj);
    }
    // <GMDX> An attachment derendered before its properties came in: ask for them (the sim still
    // answers after the local kill) so the entry learns the name a re-rez is matched on.
    if ( (LLDerenderEntry::TYPE_ATTACHMENT == pRawEntry->getType()) && (!hasKnownName(pRawEntry)) )
        LLSelectMgr::getInstance()->requestObjectPropertiesViaSelect(pObj);
    reindexNames();
    // </GMDX>

    killObject(pObj);
    return true;
}

bool LLDerenderList::addSelection(bool fPersist, std::vector<LLUUID>* pIdList)
{
    if (pIdList)
        pIdList->clear();

    // Collect first: killing objects edits the selection
    std::vector<std::pair<LLPointer<LLViewerObject>, std::string>> objects;
    std::set<const LLViewerObject*> avatars;
    LLObjectSelectionHandle hSel = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::root_iterator itObj = hSel->root_begin(); itObj != hSel->root_end(); ++itObj)
    {
        const LLSelectNode* pNode = *itObj;
        LLViewerObject* pObj = (pNode) ? pNode->getObject() : nullptr;
        if (!pObj)
            continue;
        objects.emplace_back(pObj, ((pNode->mValid) && (!pNode->mName.empty())) ? pNode->mName : LLStringUtil::null);
        if (pObj->isAvatar())
            avatars.insert(pObj);
    }

    bool fAdded = false;
    for (auto& [spObj, strName] : objects)
    {
        // Derendering an avatar takes its attachments with it
        if ( (spObj->isAttachment()) && (avatars.count(spObj->getAvatarAncestor())) )
            continue;
        if (addObjectImpl(spObj, fPersist, strName))
        {
            fAdded = true;
            if (pIdList)
            {
                const LLUUID& idObj = spObj->getID();
                auto itEntry = std::find_if(m_Entries.begin(), m_Entries.end(), [&idObj](const auto& e) { return idObj == e->m_idObject; });
                if (m_Entries.end() != itEntry)
                    pIdList->push_back((*itEntry)->getID());
            }
        }
    }

    if (fAdded)
    {
        if (fPersist)
            save();
        s_ChangeSignal();
    }
    return fAdded;
}

// static
bool LLDerenderList::canAdd(const LLViewerObject* pObj)
{
    if ( (!pObj) || (pObj->isDead()) || (!isAgentAvatarValid()) )
        return false;

    if (pObj->isAvatar())
    {
        // Another resident (never ourselves, never an animesh's control avatar: derender the animesh object instead)
        const LLVOAvatar* pAvatar = static_cast<const LLVOAvatar*>(pObj);
        return (!pAvatar->isSelf()) && (!pAvatar->isControlAvatar());
    }

    // Linkset roots only (an attachment's root is its own root edit)
    if (pObj->getRootEdit() != pObj)
        return false;

    if (pObj->isAttachment())
    {
        // Other residents' attachments only: never our own (that includes our HUDs and anything RLVa locks)
        const LLVOAvatar* pAvatar = pObj->getAvatar();
        return (pAvatar) && (!pAvatar->isSelf()) && (!pAvatar->isControlAvatar()) && (!pObj->isHUDAttachment());
    }

    // Never the object we're sitting on (RLVa @unsit, and losing our seat from under us)
    return !pObj->isChild(gAgentAvatarp);
}

// static
bool LLDerenderList::canAddSelection()
{
    LLObjectSelectionHandle hSel = LLSelectMgr::getInstance()->getSelection();
    if ( (hSel.isNull()) || (0 == hSel->getRootObjectCount()) )
        return false;
    for (LLObjectSelection::root_iterator itObj = hSel->root_begin(); itObj != hSel->root_end(); ++itObj)
    {
        if ( (*itObj) && (canAdd((*itObj)->getObject())) )
            return true;
    }
    return false;
}

bool LLDerenderList::processRootOrChild(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, U32 idParentLocal, bool fIsAvatar)
{
    // A derendered root: an object, an avatar, or an attachment (whose parent is its avatar)
    auto itId = m_BlockIds.find(idObject);
    if (m_BlockIds.end() != itId)
    {
        setRoot(itId->second, idRegion, idObjectLocal);
        return true;
    }

    // A descendant of one: child prim, attachment of a derendered avatar or one of its prims.
    // (An avatar sitting on a derendered object isn't part of it.)
    if ( (idParentLocal) && (!fIsAvatar) && (!m_BlockLocal.empty()) )
    {
        auto itLocal = m_BlockLocal.find(local_key_t(idRegion, idParentLocal));
        if (m_BlockLocal.end() != itLocal)
        {
            addChild(itLocal->second, idObjectLocal);
            return true;
        }
    }
    return false;
}

bool LLDerenderList::processObjectUpdate(U64 idRegion, const LLUUID& idObject, const LLVOCacheEntry* pCacheEntry)
{
    // Used by OUT_FULL_CACHED
    if (m_Entries.empty())
        return false;

    if (!pCacheEntry->isChild())
    {
        auto itId = m_BlockIds.find(idObject);
        if (m_BlockIds.end() == itId)
            return false;   // Not a derendered object

        // The object cache entry gives us all child prim IDs
        LLDerenderEntry* pEntry = itId->second;
        setRoot(pEntry, idRegion, pCacheEntry->getLocalID());
        for (LLVOCacheEntry::vocache_entry_set_t::const_iterator itChild = pCacheEntry->getChildrenBegin(), endChild = pCacheEntry->getChildrenEnd(); itChild != endChild; ++itChild)
            addChild(pEntry, (*itChild)->getLocalID());
        return true;
    }

    // Child prim so perform normal processing
    return processObjectUpdate(idRegion, idObject, pCacheEntry->getLocalID(), pCacheEntry->getDP()->getBuffer(), false);
}

bool LLDerenderList::processObjectUpdate(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, U32 idParentLocal, bool fIsAvatar)
{
    // Used by OUT_FULL
    if (m_Entries.empty())
        return false;
    return processRootOrChild(idRegion, idObject, idObjectLocal, idParentLocal, fIsAvatar);
}

bool LLDerenderList::processObjectUpdate(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, const U8* pBuffer, bool fIsAvatar)
{
    // Used by OUT_FULL_COMPRESSED
    if (m_Entries.empty())
        return false;

    U32 idParentLocal = 0, nMask = 0;
    htolememcpy(&nMask, pBuffer + 64, MVT_U32, 4);              // "SpecialCode"
    if (nMask & 0x20)
        htolememcpy(&idParentLocal, pBuffer + 84, MVT_U32, 4);  // "ParentID"
    return processRootOrChild(idRegion, idObject, idObjectLocal, idParentLocal, fIsAvatar);
}

void LLDerenderList::onKillObject(U64 idRegion, U32 idLocal)
{
    if (m_BlockLocal.empty())
        return;

    auto itLocal = m_BlockLocal.find(local_key_t(idRegion, idLocal));
    if (m_BlockLocal.end() == itLocal)
        return;

    // Forget the local id so the sim reusing it for something else doesn't get that blocked
    LLDerenderEntry* pEntry = itLocal->second;
    if (pEntry->idRootLocal == idLocal)
    {
        clearLocals(pEntry);
    }
    else
    {
        m_BlockLocal.erase(itLocal);
        auto& ids = pEntry->idsChildLocal;
        ids.erase(std::remove(ids.begin(), ids.end(), idLocal), ids.end());
    }
}

void LLDerenderList::onRegionRemoved(U64 idRegion)
{
    if (m_BlockLocal.empty())
        return;

    // UUID / item keys stay: the objects are re-learned from their next updates
    for (const auto& pEntry : m_Entries)
    {
        if (pEntry->idRegion == idRegion)
            clearLocals(pEntry.get());
    }
}

void LLDerenderList::onAttachmentAdded(LLVOAvatar* pAvatar, LLViewerObject* pAttachObj)
{
    if ( (m_Entries.empty()) || (!pAvatar) || (!pAttachObj) || (pAvatar->isSelf()) || (pAttachObj->isDead()) )
        return;

    LLDerenderEntry* pEntry = nullptr;
    const LLUUID& idItem = pAttachObj->getAttachmentItemID();
    if ( (idItem.notNull()) && (!m_AttachItems.empty()) )
    {
        auto itItem = m_AttachItems.find(idItem);
        if (m_AttachItems.end() != itItem)
            pEntry = itItem->second;
    }
    if (!pEntry)
        return;

    // The item was (re-)worn: block its new object UUID and local ids from here on...
    const bool fNewObject = (pEntry->m_idObject != pAttachObj->getID());
    setBlockedObject(pEntry, pAttachObj->getID());
    pEntry->idOwner = pAvatar->getID();
    if (const LLViewerRegion* pRegion = pAttachObj->getRegion())
    {
        setRoot(pEntry, pRegion->getHandle(), pAttachObj->getLocalID());
        collectLocals(pEntry, pAttachObj);
    }
    if ( (fNewObject) && (pEntry->isPersistent()) )
        save();

    // ...and kill it once the update that created it has been fully processed
    kill_derendered_on_idle(pAttachObj->getID()); // <GMDX> shared with the name match
}

// <GMDX>
bool LLDerenderList::wantsProperties(const LLUUID& idWearer, const LLUUID& idObject) const
{
    if (m_Entries.empty())
        return false;

    // Blocked: only an entry made before its name was known learns it from the reply
    auto itId = m_BlockIds.find(idObject);
    if (m_BlockIds.end() != itId)
        return (LLDerenderEntry::TYPE_ATTACHMENT == itId->second->getType()) && (!hasKnownName(itId->second));

    auto itOwner = m_AttachNames.find(idWearer);
    if (m_AttachNames.end() == itOwner)
        return false;
    for (const auto& [strName, entries] : itOwner->second)
    {
        if (entries.end() != std::find_if(entries.begin(), entries.end(), &LLDerenderList::canRematch))
            return true;
    }
    return false;
}

void LLDerenderList::onObjectProperties(const LLUUID& idObject, const LLUUID& idOwner, const std::string& strName)
{
    if ( (m_Entries.empty()) || (idObject.isNull()) || (idOwner.isNull()) || (strName.empty()) )
        return;

    // An attachment we block whose name was not known yet (derendered before its properties
    // arrived, or an entry from before names were recorded): remember it from now on
    auto itId = m_BlockIds.find(idObject);
    if (m_BlockIds.end() != itId)
    {
        LLDerenderEntry* pEntry = itId->second;
        if ( (LLDerenderEntry::TYPE_ATTACHMENT == pEntry->getType()) && (!hasKnownName(pEntry)) )
        {
            pEntry->m_strEntryName = strName;
            if (pEntry->idOwner.isNull())
                pEntry->idOwner = idOwner;
            reindexNames();
            if (pEntry->isPersistent())
                save();
            s_ChangeSignal();
        }
        return;
    }

    if (m_AttachNames.empty())
        return;
    auto itOwner = m_AttachNames.find(idOwner);
    if (m_AttachNames.end() == itOwner)
        return;
    auto itName = itOwner->second.find(strName);
    if (itOwner->second.end() == itName)
        return;
    // Several attachments of one wearer can share a name: take one whose own object is gone
    auto itEntry = std::find_if(itName->second.begin(), itName->second.end(), &LLDerenderList::canRematch);
    if (itName->second.end() == itEntry)
        return;
    LLDerenderEntry* pEntry = *itEntry;

    // Only the root of an attachment the entry's owner is wearing right now (any reply lands
    // here: a child prim or an in-world object of theirs with the same name must not match)
    LLViewerObject* pObj = gObjectList.findObject(idObject);
    if ( (!pObj) || (pObj->isDead()) || (!pObj->isAttachment()) || (pObj->getRootEdit() != pObj) )
        return;
    LLVOAvatar* pAvatar = pObj->getAvatarAncestor();
    if ( (!pAvatar) || (pAvatar->isSelf()) || (pAvatar->getID() != idOwner) )
        return;

    // Same attachment, new identity: key the entry on what identifies it now, so the next
    // re-wear of this item is caught by its item id on arrival, before it is ever drawn
    const LLUUID& idItem = pObj->getAttachmentItemID();
    const LLUUID idNewEntry = (idItem.notNull()) ? idItem : idObject;
    if (idNewEntry != pEntry->getID())
    {
        if (findEntry(idNewEntry))
            return;
        if (pEntry->isKeyedOnItem())
        {
            auto itItem = m_AttachItems.find(pEntry->getID());
            if ( (m_AttachItems.end() != itItem) && (itItem->second == pEntry) )
                m_AttachItems.erase(itItem);
        }
        pEntry->m_idEntry = idNewEntry;
        pEntry->m_fItemKey = idItem.notNull();
        if (pEntry->m_fItemKey)
            m_AttachItems[idItem] = pEntry;
    }
    setBlockedObject(pEntry, idObject);
    if (const LLViewerJointAttachment* pAttachPt = pAvatar->getTargetAttachmentPoint(pObj))
        pEntry->strAttachPt = pAttachPt->getName();
    if (const LLViewerRegion* pRegion = pObj->getRegion())
    {
        setRoot(pEntry, pRegion->getHandle(), pObj->getLocalID());
        collectLocals(pEntry, pObj);
    }
    save();
    s_ChangeSignal();

    kill_derendered_on_idle(idObject);
}
// </GMDX>

// ============================================================================
