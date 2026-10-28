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
 * Slipstream (#202 / #426): extended to avatars and other residents' attachments,
 * with Firestorm-style ergonomics (session derender vs. permanent blacklist, see
 * Firestorm's llviewermenu.cpp derenderObject() / fsassetblacklist.cpp). Our own
 * implementation; Firestorm's released source was used as a behavioural reference.
 *
 */
#ifndef LL_DERENDERLIST_H
#define LL_DERENDERLIST_H

#include "llsingleton.h"
#include "lluuid.h"
#include "lldate.h"
#include "v3math.h"

#include <boost/signals2.hpp>
#include <boost/unordered_map.hpp>
#include <map> // <GMDX>

// ============================================================================
// Forward declarations
//

class LLVOAvatar;
class LLVOCacheEntry;
class LLViewerObject;

// ============================================================================
// LLDerenderEntry
//
// One derendered thing. What it is keyed on depends on the type:
//   TYPE_OBJECT     - an in-world linkset, keyed on its root prim's UUID
//   TYPE_AVATAR     - another resident (and with it all of their attachments), keyed on the agent id
//   TYPE_ATTACHMENT - one of another resident's attachments. Attachment object UUIDs change every
//                     time the item is worn, so the entry is keyed on the wearer's inventory item id
//                     (the "AttachItemID" name value) when the sim provided one, which survives
//                     re-wearing; otherwise on the object UUID (then it lasts until the item is re-worn).
//
// Runtime state (not persisted): the region + local ids we block object updates for. They are
// (re)learned from the object updates themselves, so persisted entries work after a relog.
//

class LLDerenderEntry
{
public:
    // NOTE: the numeric values are persisted, don't reorder
    enum EEntryType
    {
        TYPE_OBJECT = 0,
        TYPE_AVATAR = 1,
        TYPE_ALL = 2,
        TYPE_NONE = 3,
        TYPE_ATTACHMENT = 4,
    };

    enum
    {
        FLAG_NONE    = 0x00,
        FLAG_PERSIST = 0x01
    };

    LLDerenderEntry(EEntryType eType, bool fPersist);
    explicit LLDerenderEntry(const LLSD& sdData);

    const std::string& getName() const          { return m_strEntryName; }
    const LLUUID&      getID() const            { return m_idEntry; }
    EEntryType         getType() const          { return m_eType; }
    bool               isPersistent() const     { return m_nFlags & FLAG_PERSIST; }
    void               setPersistent(bool fPersist);
    bool               isValid() const          { return (m_eType == TYPE_OBJECT || m_eType == TYPE_AVATAR || m_eType == TYPE_ATTACHMENT) && m_idEntry.notNull(); }
    bool               isKeyedOnItem() const    { return m_fItemKey; }

    LLSD toLLSD() const;

public:
    // Persisted
    EEntryType  m_eType = TYPE_NONE;
    S32         m_nFlags = FLAG_NONE;
    LLUUID      m_idEntry;              // see the class comment
    std::string m_strEntryName;         // object name / avatar legacy name
    LLDate      m_Date;                 // when it was derendered
    std::string strRegionName;          // region at the time it was derendered
    LLVector3   posRegion;              // position at the time it was derendered (objects)
    LLUUID      idOwner;                // TYPE_ATTACHMENT: the wearer
    std::string strAttachPt;            // TYPE_ATTACHMENT: attachment point name
    bool        m_fItemKey = false;     // TYPE_ATTACHMENT: m_idEntry is the wearer's inventory item id

    // Runtime
    LLUUID              m_idObject;     // UUID object updates are blocked for (root prim / avatar / current attachment)
    U64                 idRegion = 0;   // region handle of the root (0 = not seen yet this session)
    U32                 idRootLocal = 0;// local id of the root (prim / avatar / attachment root)
    std::vector<U32>    idsChildLocal;  // blocked descendants (child prims, an avatar's attachments and their prims)
    bool                m_fItemSeen = false; // <GMDX> TYPE_ATTACHMENT: this item id was seen worn this session (setRoot)
};

// ============================================================================
// LLDerenderList
//

class LLDerenderList : public LLSingleton<LLDerenderList>
{
    LLSINGLETON(LLDerenderList);
protected:
    /*virtual*/ ~LLDerenderList();

    void load();
    void save() const;

    /*
     * Entry helper functions
     */
public:
    using entry_list_t = std::list<std::unique_ptr<LLDerenderEntry>>;

    const entry_list_t& getEntries() const { return m_Entries; }
    const LLDerenderEntry* getEntry(const LLUUID& idEntry) const;
    // Removes the entries (any type) and re-requests whatever we know of them from the sim
    void removeEntries(const uuid_vec_t& idsEntry);
    // Session <-> permanent
    void setPersistent(const uuid_vec_t& idsEntry, bool fPersist);
    // Legacy interface (Catznip)
    void removeObject(LLDerenderEntry::EEntryType eType, const LLUUID& idObject) { removeEntries(uuid_vec_t{ idObject }); }
    void removeObjects(LLDerenderEntry::EEntryType eType, const uuid_vec_t& idsObject) { removeEntries(idsObject); }

    /*
     * Object helper functions
     */
public:
    // Derenders every derenderable root of the current selection
    bool              addSelection(bool fPersist, std::vector<LLUUID>* pIdList = nullptr);
    // Derenders one object (a linkset root, another resident's attachment root or another avatar)
    bool              addObject(LLViewerObject* pObj, bool fPersist, const std::string& strName = LLStringUtil::null);
    static bool       canAdd(const LLViewerObject* pObj);
    static bool       canAddSelection();
    bool              isDerendered(const LLUUID& idObject) const { return m_BlockIds.end() != m_BlockIds.find(idObject); }

    // Object update hooks: return true if the object must not be created
    bool              processObjectUpdate(U64 idRegion, const LLUUID& idObject, const LLVOCacheEntry* pEntry);                           // OUT_FULL_CACHED
    bool              processObjectUpdate(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, U32 idParentLocal, bool fIsAvatar); // OUT_FULL
    bool              processObjectUpdate(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, const U8* pBuffer, bool fIsAvatar);  // OUT_FULL_COMPRESSED
    // The sim killed a local id (a blocked object can't be found by UUID so the kill would otherwise be lost)
    void              onKillObject(U64 idRegion, U32 idLocal);
    // The region is going away (left / restarted): its local ids will be reassigned
    void              onRegionRemoved(U64 idRegion);
    // Another resident attached something: catches re-worn attachments of item-keyed entries
    void              onAttachmentAdded(LLVOAvatar* pAvatar, LLViewerObject* pAttachObj);
    // <GMDX> An attachment's properties are worth asking for (GMDXAttachProbe): it is blocked
    // and its entry's name is still unknown, or its wearer has a permanent entry that only
    // wearer + name can find again and that has not been matched yet.
    bool              wantsProperties(const LLUUID& idWearer, const LLUUID& idObject) const;
    // <GMDX> An ObjectProperties reply. Fills in the name of a blocked attachment derendered
    // before its name was known, and re-points a permanent attachment entry whose object came
    // back with a new UUID (re-rez, relog, re-copied item) matched on wearer + object name.
    void              onObjectProperties(const LLUUID& idObject, const LLUUID& idOwner, const std::string& strName);

    // Kills an object locally the way process_kill_object() does
    static void       killObject(LLViewerObject* pObj);

protected:
    LLDerenderEntry*  findEntry(const LLUUID& idEntry) const;
    bool              addObjectImpl(LLViewerObject* pObj, bool fPersist, const std::string& strName);
    void              addEntry(std::unique_ptr<LLDerenderEntry> pEntry);
    void              setBlockedObject(LLDerenderEntry* pEntry, const LLUUID& idObject);
    void              setRoot(LLDerenderEntry* pEntry, U64 idRegion, U32 idRootLocal);
    void              addChild(LLDerenderEntry* pEntry, U32 idLocal);
    void              clearLocals(LLDerenderEntry* pEntry);
    void              collectLocals(LLDerenderEntry* pEntry, const LLViewerObject* pObj);
    bool              processRootOrChild(U64 idRegion, const LLUUID& idObject, U32 idObjectLocal, U32 idParentLocal, bool fIsAvatar);
    // <GMDX> owner + name index of the permanent attachment entries (rebuilt on change; user-sized)
    void              reindexNames();
    static bool       hasKnownName(const LLDerenderEntry* pEntry);
    // <GMDX> A name match may move this entry onto another object: its own object is not in the
    // sim now (idRootLocal is cleared by the sim's KillObject), and it is not an item the sim
    // already showed worn this session (a second attachment with the same name is another item)
    static bool       canRematch(const LLDerenderEntry* pEntry) { return (0 == pEntry->idRootLocal) && (!pEntry->isKeyedOnItem() || !pEntry->m_fItemSeen); }

    /*
     * Static member functions
     */
public:
    typedef boost::signals2::signal<void()> change_signal_t;
    static boost::signals2::connection setChangeCallback(const change_signal_t::slot_type& cb) { return s_ChangeSignal.connect(cb); }

protected:
    entry_list_t m_Entries;

    using local_key_t = std::pair<U64, U32>;                                    // region handle, local id
    boost::unordered_map<LLUUID, LLDerenderEntry*>      m_BlockIds;             // object UUID -> entry
    boost::unordered_map<LLUUID, LLDerenderEntry*>      m_AttachItems;          // attachment item id -> entry
    boost::unordered_map<local_key_t, LLDerenderEntry*> m_BlockLocal;           // blocked root/descendant local ids -> entry
    std::map<LLUUID, std::map<std::string, std::vector<LLDerenderEntry*>>> m_AttachNames; // <GMDX> wearer -> object name -> permanent attachment entries

    static change_signal_t  s_ChangeSignal;
    static std::string      s_PersistFilename;
};

// ============================================================================

#endif // LL_DERENDERLIST_H
