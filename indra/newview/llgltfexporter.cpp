/**
 * @file llgltfexporter.cpp
 * @brief Export the selected linkset(s) to glTF 2.0 (#908).
 *
 * PERMISSION MODEL (deny unless every check is proven; nothing is fetched or
 * written before the whole selection passes):
 *
 *  Per prim, from the region's ObjectProperties (LLSelectNode::mPermissions,
 *  waited for until LLSelectNode::mValid):
 *   - owner == you, and not group-owned;
 *   - you are the creator, OR your owner mask has MODIFY|COPY|TRANSFER;
 *   - attachments and rigged mesh: you must be the CREATOR (full perm is not
 *     enough), and the attachment must be on your own avatar;
 *   - avatars, objects attached to other avatars, trees/grass, glTF scene
 *     objects: never.
 *
 *  Per asset the prim uses -- diffuse texture, normal map, specular map, PBR
 *  material asset, every texture of the final (base + override) PBR material,
 *  sculpt map: the viewer only learns asset permissions through inventory, so
 *  the asset must be proven by an inventory item with that asset id that is
 *   - in your inventory (not a link), owned by you, and created by you or
 *     full-permission (owner mask MODIFY|COPY|TRANSFER), or
 *   - in the Library with full next-owner permissions,
 *  or be one of the viewer's built-in default textures (plywood, blank,
 *  transparent, default normal/specular, default sculpt, blank material).
 *  Being the prim's creator does NOT vouch for its textures (anyone can apply
 *  any texture they hold). If inventory is not fully loaded the exporter starts
 *  the background fetch and waits; an asset still unproven afterwards denies.
 *  Local (on-disk preview) textures/materials/meshes and Bakes-on-Mesh deny.
 *
 *  Mesh asset: mesh assets are not inventory items, so the mesh is covered by the
 *  prim check (the creator of an uploaded mesh object is the uploader; otherwise
 *  the prim must be full-permission), with rigged mesh requiring creator == you.
 *  A mesh counts as rigged when the object has skin info, OR the mesh header
 *  lists a skin, OR the header is not loaded yet (unknown -> the stricter rule).
 *
 *  Not used: LLSelectNode::mAggregateTexturePerm*, which the region computes
 *  over the TEXTURES IN THE OBJECT'S CONTENTS, not the textures on its faces.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
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

#include "llgltfexporter.h"
#include "llgltfexportwriter.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llavatarappearancedefines.h"
#include "llcallbacklist.h"
#include "llcleanmeshasset.h"
#include "lldir.h"
#include "llfetchedgltfmaterial.h"
#include "llfilepicker.h"
#include "llfloaterreg.h"
#include "llgltfmateriallist.h"
#include "llimagepng.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "lllocalbitmaps.h"
#include "lllocalgltfmaterials.h"
#include "lllocalmesh.h"
#include "llmaterial.h"
#include "llmeshrepository.h"
#include "llnotificationsutil.h"
#include "llselectmgr.h"
#include "lltexturectrl.h"      // get_is_predefined_texture
#include "lltextureentry.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"

#include <boost/algorithm/string/join.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>

namespace
{
namespace gx = LLGLTFExport;

constexpr F32 PROPERTIES_TIMEOUT = 20.f;   // ObjectProperties for every prim
constexpr F32 MATERIAL_TIMEOUT   = 30.f;   // PBR material assets
constexpr F32 INVENTORY_TIMEOUT  = 90.f;   // full inventory fetch (only if needed)
constexpr F32 ASSET_STALL_TIMEOUT = 120.f; // no texture/mesh completed for this long
constexpr F32 MESH_REREQUEST_SECS = 15.f;

std::string tr(const std::string& key, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
{
    return LLTrans::getString(key, args);
}

// ---------------------------------------------------------------------------
// Inventory proof of asset permissions
// ---------------------------------------------------------------------------
class LLAssetSetMatches : public LLInventoryCollectFunctor
{
public:
    explicit LLAssetSetMatches(const std::set<LLUUID>& ids) : mIDs(ids) {}
    bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
    {
        return item && !item->getIsLinkType() && mIDs.count(item->getAssetUUID());
    }
private:
    const std::set<LLUUID>& mIDs;
};

enum EAssetKind
{
    KIND_TEXTURE, KIND_NORMAL, KIND_SPECULAR, KIND_MATERIAL, KIND_PBR_TEXTURE, KIND_SCULPT
};

const char* kind_string_key(EAssetKind k)
{
    switch (k)
    {
    case KIND_TEXTURE:     return "GLTFExportKindTexture";
    case KIND_NORMAL:      return "GLTFExportKindNormal";
    case KIND_SPECULAR:    return "GLTFExportKindSpecular";
    case KIND_MATERIAL:    return "GLTFExportKindMaterial";
    case KIND_PBR_TEXTURE: return "GLTFExportKindPBRTexture";
    case KIND_SCULPT:      return "GLTFExportKindSculpt";
    }
    return "GLTFExportKindTexture";
}

bool is_texture_kind(EAssetKind k) { return k != KIND_MATERIAL; }

struct AssetRef
{
    LLUUID     id;
    EAssetKind kind;
};

// ---------------------------------------------------------------------------
// Per-prim snapshot
// ---------------------------------------------------------------------------
enum EGeomKind { GEOM_PRIM, GEOM_SCULPT, GEOM_MESH };

struct PrimInfo
{
    LLPointer<LLViewerObject> obj;
    LLPointer<LLViewerObject> root;
    std::string name;
    std::string desc;
    S32         link_number = 0;
    LLPermissions perm;
    bool        valid = false;      // ObjectProperties arrived and copied

    EGeomKind     geom = GEOM_PRIM;
    LLVolumeParams params;
    LLUUID        sculpt_id;        // sculpt map or mesh asset id
    S32           mesh_lod = -1;    // target LOD once the header is known
    bool          mesh_requested = false;
    bool          mesh_failed = false;
    bool          rigged = false;   // permission rule: creator only
    bool          bind_needed = false; // drawn skinned (attachment / animesh): export in bind pose
    bool          has_bind = false;
    F32           bind_shape[16];   // LLMeshSkinInfo::mBindShapeMatrix once known
    F32           mesh_last_request = 0.f;
    LLPointer<LLVolume> volume;     // private, fully generated volume (export source)

    std::vector<AssetRef> assets;   // everything the permission check covers
};

struct TextureFetch
{
    LLPointer<LLViewerFetchedTexture> tex;
    S32  old_boost = 0;
    LLPointer<LLImageRaw> raw;      // full-resolution copy once delivered
    bool failed = false;
    bool done = false;
    std::string why;
};

struct CallbackData
{
    U32    serial;
    LLUUID id;
};

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------
class ExportJob
{
public:
    enum EState
    {
        ST_PROPERTIES, ST_MATERIALS, ST_INVENTORY, ST_CHECK, ST_PICK, ST_PICKING, ST_FETCH, ST_WRITE, ST_DONE
    };

    explicit ExportJob(U32 serial) : mSerial(serial) {}
    ~ExportJob();

    bool init();                    // snapshot the selection; false = nothing exportable (notified)
    bool tick();                    // true = finished (success, denial, failure or cancel)

    void onTexture(const LLUUID& id, bool success, LLImageRaw* src, bool final);
    void onFilePicked(const std::string& path);
    void onPickCancelled() { mState = ST_DONE; }

    std::string statusText() const;
    F32 progress() const;
    U32 serial() const { return mSerial; }

private:
    bool tickProperties();
    bool tickMaterials();
    bool tickInventory();
    bool checkPermissions();        // false = denied (notified)
    void startPicker();
    void startFetch();
    bool tickFetch();
    bool tickMesh(PrimInfo& p);
    bool buildSculpt(PrimInfo& p);
    bool writeFile();

    void collectAssets(PrimInfo& p);
    bool proveAsset(const AssetRef& a, std::string& reason);
    void fail(const std::string& reason);
    bool objectsAlive();

    U32 mSerial;
    EState mState = ST_PROPERTIES;
    LLFrameTimer mTimer;            // per-state timeout
    LLFrameTimer mStallTimer;       // asset-fetch progress
    LLFrameTimer mScanTimer;        // inventory rescan throttle
    bool mSentSelect = false;
    bool mInventoryFetchStarted = false;

    std::vector<PrimInfo> mPrims;
    std::vector<LLPointer<LLViewerObject> > mRoots; // linkset roots, primary first
    std::string mRootName;
    std::string mPath;

    std::map<LLUUID, LLPointer<LLFetchedGLTFMaterial> > mMaterials;
    std::map<LLUUID, bool> mProofItems;  // asset id -> proven by an inventory item
    std::map<LLUUID, TextureFetch> mTextures;
    LLLoadedCallbackEntry::source_callback_list_t mCallbackList;
    S32 mAssetsTotal = 0;
    std::vector<std::string> mReduced;  // textures delivered below full resolution
};

std::unique_ptr<ExportJob> sJob;
U32 sSerial = 0;

void loaded_callback(bool success, LLViewerFetchedTexture* src_vi, LLImageRaw* src, LLImageRaw* /*aux*/,
                     S32 /*discard*/, bool final, void* userdata)
{
    CallbackData* data = (CallbackData*)userdata;
    if (sJob && sJob->serial() == data->serial)
    {
        sJob->onTexture(data->id, success, src, final);
    }
    if (final || !success)
    {
        delete data;
    }
}

ExportJob::~ExportJob()
{
    // Synchronously calls loaded_callback(false, ..., final=true) for every pending
    // entry; sJob no longer points here by then, so they only free their data.
    LLLoadedCallbackEntry::cleanUpCallbackList(&mCallbackList);
    for (auto& [id, t] : mTextures)
    {
        if (t.tex.notNull())
        {
            t.tex->setBoostLevel(t.old_boost);
        }
    }
}

void ExportJob::fail(const std::string& reason)
{
    LL_WARNS("GLTFExport") << "export failed: " << reason << LL_ENDL;
    LLSD args;
    args["REASON"] = reason;
    LLNotificationsUtil::add("GLTFExportFailed", args);
    mState = ST_DONE;
}

bool ExportJob::objectsAlive()
{
    for (const PrimInfo& p : mPrims)
    {
        if (p.obj.isNull() || p.obj->isDead())
        {
            return false;
        }
    }
    return true;
}

S32 link_number_of(LLViewerObject* obj)
{
    LLViewerObject* root = obj->getRootEdit();
    if (!root) return 0;
    const LLViewerObject::const_child_list_t& children = root->getChildren();
    if (obj == root) return children.empty() ? 0 : 1;
    S32 index = 1;
    for (LLViewerObject* child : children)
    {
        ++index;
        if (child == obj) return index;
    }
    return 0;
}

bool ExportJob::init()
{
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* obj = node ? node->getObject() : nullptr;
        if (!obj || obj->isDead()) continue;

        PrimInfo p;
        p.obj = obj;
        p.root = obj->getRootEdit();
        p.link_number = link_number_of(obj);
        mPrims.push_back(p);
    }
    if (mPrims.empty())
    {
        LLNotificationsUtil::add("GLTFExportNothing");
        return false;
    }
    // Linkset roots, in selection order (the primary root first).
    LLViewerObject* primary = sel->getFirstRootObject(true);
    if (primary) mRoots.push_back(primary->getRootEdit());
    for (const PrimInfo& p : mPrims)
    {
        if (std::find(mRoots.begin(), mRoots.end(), p.root) == mRoots.end())
        {
            mRoots.push_back(p.root);
        }
    }
    mTimer.reset();
    return true;
}

// Wait for ObjectProperties (owner / creator / permissions / name) of every prim.
bool ExportJob::tickProperties()
{
    if (!objectsAlive())
    {
        fail(tr("GLTFExportFailGone"));
        return true;
    }
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    bool all_valid = true;
    for (PrimInfo& p : mPrims)
    {
        if (p.valid) continue;
        LLSelectNode* node = sel->findNode(p.obj);
        if (!node)
        {
            fail(tr("GLTFExportFailSelectionChanged"));
            return true;
        }
        if (node->mValid && node->mPermissions)
        {
            p.perm = *node->mPermissions;
            p.name = node->mName;
            p.desc = node->mDescription;
            p.valid = true;
        }
        else
        {
            all_valid = false;
        }
    }
    if (all_valid)
    {
        const PrimInfo* rootp = nullptr;
        for (const PrimInfo& p : mPrims)
        {
            if (!mRoots.empty() && p.obj == mRoots.front()) rootp = &p;
        }
        mRootName = rootp ? rootp->name : mPrims.front().name;
        for (PrimInfo& p : mPrims)
        {
            collectAssets(p);
        }
        mState = ST_MATERIALS;
        mTimer.reset();
        return false;
    }
    if (!mSentSelect && mTimer.getElapsedTimeF32() > 2.f)
    {
        // Ask the region again for the selection's properties.
        LLSelectMgr::getInstance()->sendSelect();
        mSentSelect = true;
    }
    if (mTimer.getElapsedTimeF32() > PROPERTIES_TIMEOUT)
    {
        std::vector<std::string> lines;
        for (const PrimInfo& p : mPrims)
        {
            if (p.valid) continue;
            LLStringUtil::format_map_t args;
            args["[PRIM]"] = p.obj->getID().asString();
            args["[LINK]"] = llformat("%d", p.link_number);
            args["[REASON]"] = tr("GLTFExportReasonNoProperties");
            lines.push_back(tr("GLTFExportDeniedEntry", args));
        }
        LLSD args;
        args["REASONS"] = boost::algorithm::join(lines, "\n");
        LLNotificationsUtil::add("GLTFExportDenied", args);
        mState = ST_DONE;
        return true;
    }
    return false;
}

// Records every asset the permission check must prove for this prim. PBR texture
// ids are added in tickMaterials() once the material assets are loaded.
void ExportJob::collectAssets(PrimInfo& p)
{
    LLViewerObject* obj = p.obj;
    if (obj->getPCode() != LL_PCODE_VOLUME || !obj->getVolume())
    {
        return; // denied in checkPermissions
    }
    p.params = obj->getVolume()->getParams();
    const U8 sculpt_type = p.params.getSculptType() & LL_SCULPT_TYPE_MASK;
    p.sculpt_id = p.params.getSculptID();
    if (sculpt_type == LL_SCULPT_TYPE_MESH)
    {
        p.geom = GEOM_MESH;
        LLVOVolume* vobj = (LLVOVolume*)obj;
        p.rigged = vobj->isRiggedMesh() || gMeshRepo.hasSkinInfo(p.sculpt_id) || !gMeshRepo.hasHeader(p.sculpt_id);
        p.bind_needed = (obj->isAttachment() || obj->isAnimatedObject())
            && (vobj->isRiggedMesh() || gMeshRepo.hasSkinInfo(p.sculpt_id));
    }
    else if (p.params.isSculpt() && sculpt_type != LL_SCULPT_TYPE_GLTF && sculpt_type != LL_SCULPT_TYPE_NONE)
    {
        p.geom = GEOM_SCULPT;
        p.assets.push_back({ p.sculpt_id, KIND_SCULPT });
    }

    const S32 num_tes = obj->getNumTEs();
    for (S32 te = 0; te < num_tes; ++te)
    {
        const LLTextureEntry* tep = obj->getTE((U8)te);
        if (!tep) continue;
        const LLUUID& mat_id = obj->getRenderMaterialID((U8)te);
        if (mat_id.notNull())
        {
            p.assets.push_back({ mat_id, KIND_MATERIAL });
            if (!mMaterials.count(mat_id))
            {
                mMaterials[mat_id] = gGLTFMaterialList.getMaterial(mat_id);
            }
            continue; // the legacy texture underneath is not rendered and not exported
        }
        p.assets.push_back({ tep->getID(), KIND_TEXTURE });
        if (const LLMaterial* lm = tep->getMaterialParams().get())
        {
            if (lm->getNormalID().notNull()) p.assets.push_back({ lm->getNormalID(), KIND_NORMAL });
            if (lm->getSpecularID().notNull()) p.assets.push_back({ lm->getSpecularID(), KIND_SPECULAR });
        }
    }
}

// Final (base + override) PBR material of a face, as LL renders it.
bool render_material(LLViewerObject* obj, S32 te, const std::map<LLUUID, LLPointer<LLFetchedGLTFMaterial> >& mats, LLGLTFMaterial& out)
{
    const LLUUID& mat_id = obj->getRenderMaterialID((U8)te);
    auto it = mats.find(mat_id);
    if (mat_id.isNull() || it == mats.end() || it->second.isNull() || !it->second->isLoaded())
    {
        return false;
    }
    out = *it->second;
    const LLTextureEntry* tep = obj->getTE((U8)te);
    if (tep && tep->getGLTFMaterialOverride())
    {
        out.applyOverride(*tep->getGLTFMaterialOverride());
    }
    return true;
}

bool ExportJob::tickMaterials()
{
    bool pending = false;
    for (auto& [id, mat] : mMaterials)
    {
        if (mat.notNull() && mat->isFetching()) pending = true;
    }
    if (pending && mTimer.getElapsedTimeF32() < MATERIAL_TIMEOUT)
    {
        return false;
    }
    // Add the textures of every final material (failed materials deny in the check).
    for (PrimInfo& p : mPrims)
    {
        if (p.obj->getPCode() != LL_PCODE_VOLUME) continue;
        for (S32 te = 0; te < p.obj->getNumTEs(); ++te)
        {
            LLGLTFMaterial mat;
            if (!render_material(p.obj, te, mMaterials, mat)) continue;
            for (U32 i = 0; i < LLGLTFMaterial::GLTF_TEXTURE_INFO_COUNT; ++i)
            {
                if (mat.mTextureId[i].notNull())
                {
                    p.assets.push_back({ mat.mTextureId[i], KIND_PBR_TEXTURE });
                }
            }
        }
    }
    mState = ST_INVENTORY;
    mTimer.reset();
    return false;
}

// Builds the asset -> proving inventory item map. Waits for the background
// inventory fetch only when something is not proven yet.
bool ExportJob::tickInventory()
{
    // A full inventory scan per attempt; while waiting on the background fetch,
    // rescan every couple of seconds, not every frame.
    if (mInventoryFetchStarted && mScanTimer.getElapsedTimeF32() < 2.f)
    {
        return false;
    }
    mScanTimer.reset();
    std::set<LLUUID> wanted;
    for (const PrimInfo& p : mPrims)
    {
        for (const AssetRef& a : p.assets)
        {
            if (a.id.notNull() && !get_is_predefined_texture(a.id)) wanted.insert(a.id);
        }
    }
    mProofItems.clear();
    if (!wanted.empty() && gInventory.isInventoryUsable())
    {
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        LLAssetSetMatches matches(wanted);
        gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::INCLUDE_TRASH, matches);
        if (gInventory.getLibraryRootFolderID().notNull())
        {
            gInventory.collectDescendentsIf(gInventory.getLibraryRootFolderID(), cats, items, LLInventoryModel::INCLUDE_TRASH, matches);
        }
        for (LLViewerInventoryItem* item : items)
        {
            // Keep the first item that actually proves the asset.
            mProofItems.emplace(item->getAssetUUID(), false);
            const LLPermissions& perm = item->getPermissions();
            const LLUUID& agent = gAgent.getID();
            bool proves = false;
            if (gInventory.isObjectDescendentOf(item->getUUID(), gInventory.getLibraryRootFolderID()))
            {
                proves = (perm.getMaskNextOwner() & PERM_ITEM_UNRESTRICTED) == PERM_ITEM_UNRESTRICTED;
            }
            else if (perm.getOwner() == agent && !perm.isGroupOwned())
            {
                proves = perm.getCreator() == agent
                    || (perm.getMaskOwner() & PERM_ITEM_UNRESTRICTED) == PERM_ITEM_UNRESTRICTED;
            }
            if (proves)
            {
                mProofItems[item->getAssetUUID()] = true;
            }
        }
    }

    bool all_proven = true;
    for (const LLUUID& id : wanted)
    {
        auto it = mProofItems.find(id);
        if (it == mProofItems.end() || !it->second) all_proven = false;
    }

    LLInventoryModelBackgroundFetch& fetch = LLInventoryModelBackgroundFetch::instance();
    if (!all_proven && !fetch.isEverythingFetched() && mTimer.getElapsedTimeF32() < INVENTORY_TIMEOUT)
    {
        if (!mInventoryFetchStarted)
        {
            fetch.start();
            mInventoryFetchStarted = true;
        }
        return false; // re-scan next frame
    }
    mState = ST_CHECK;
    return false;
}

bool ExportJob::proveAsset(const AssetRef& a, std::string& reason)
{
    LLStringUtil::format_map_t args;
    args["[KIND]"] = tr(kind_string_key(a.kind));
    args["[ASSET]"] = a.id.asString();
    if (a.id.isNull() || get_is_predefined_texture(a.id))
    {
        return true;
    }
    if (LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(a.id))
    {
        reason = tr("GLTFExportReasonAssetBaked", args);
        return false;
    }
    if ((is_texture_kind(a.kind) && LLLocalBitmapMgr::getInstance()->isLocal(a.id))
        || (a.kind == KIND_MATERIAL && LLLocalGLTFMaterialMgr::getInstance()->isLocal(a.id)))
    {
        reason = tr("GLTFExportReasonAssetLocal", args);
        return false;
    }
    auto it = mProofItems.find(a.id);
    if (it == mProofItems.end() || !it->second)
    {
        reason = tr("GLTFExportReasonAssetUnproven", args);
        return false;
    }
    if (a.kind == KIND_MATERIAL && mMaterials.count(a.id)
        && (mMaterials[a.id].isNull() || !mMaterials[a.id]->isLoaded()))
    {
        reason = tr("GLTFExportReasonMaterialFailed", args);
        return false;
    }
    return true;
}

bool ExportJob::checkPermissions()
{
    const LLUUID& agent = gAgent.getID();
    std::vector<std::string> lines;
    for (PrimInfo& p : mPrims)
    {
        std::vector<std::string> reasons;
        LLViewerObject* obj = p.obj;
        LLVOAvatar* av = obj->getAvatar();
        const bool rigged = p.rigged || (obj->getPCode() == LL_PCODE_VOLUME && ((LLVOVolume*)obj)->isRiggedMesh());

        if (obj->asAvatar() || (obj->isAttachment() && av != gAgentAvatarp.get()))
        {
            reasons.push_back(tr("GLTFExportReasonOtherAvatar"));
        }
        else if (obj->getPCode() != LL_PCODE_VOLUME)
        {
            reasons.push_back(tr("GLTFExportReasonNotPrim"));
        }
        else if (!obj->getVolume())
        {
            reasons.push_back(tr("GLTFExportReasonNoVolume"));
        }
        else if ((p.params.getSculptType() & LL_SCULPT_TYPE_MASK) == LL_SCULPT_TYPE_GLTF || obj->mGLTFAsset)
        {
            reasons.push_back(tr("GLTFExportReasonGLTFScene"));
        }
        else if (p.perm.isGroupOwned())
        {
            reasons.push_back(tr("GLTFExportReasonGroupOwned"));
        }
        else if (p.perm.getOwner() != agent || !obj->permYouOwner())
        {
            reasons.push_back(tr("GLTFExportReasonNotOwner"));
        }
        else if ((obj->isAttachment() || rigged) && p.perm.getCreator() != agent)
        {
            reasons.push_back(tr("GLTFExportReasonAttachmentCreator"));
        }
        else if (p.perm.getCreator() != agent
                 && (p.perm.getMaskOwner() & PERM_ITEM_UNRESTRICTED) != PERM_ITEM_UNRESTRICTED)
        {
            reasons.push_back(tr("GLTFExportReasonNotFullPerm"));
        }

        if (reasons.empty() && p.geom == GEOM_MESH && LLLocalMeshMgr::instanceExists()
            && LLLocalMeshMgr::getInstance()->isLocal(p.sculpt_id))
        {
            LLStringUtil::format_map_t args;
            args["[KIND]"] = tr("GLTFExportKindMesh");
            args["[ASSET]"] = p.sculpt_id.asString();
            reasons.push_back(tr("GLTFExportReasonAssetLocal", args));
        }

        if (reasons.empty())
        {
            std::set<LLUUID> seen;
            for (const AssetRef& a : p.assets)
            {
                if (!seen.insert(a.id).second) continue;
                std::string why;
                if (!proveAsset(a, why)) reasons.push_back(why);
            }
        }

        for (const std::string& r : reasons)
        {
            LLStringUtil::format_map_t args;
            args["[PRIM]"] = p.name.empty() ? obj->getID().asString() : p.name;
            args["[LINK]"] = llformat("%d", p.link_number);
            args["[REASON]"] = r;
            lines.push_back(tr("GLTFExportDeniedEntry", args));
        }
    }
    if (!lines.empty())
    {
        constexpr size_t MAX_LINES = 20;
        if (lines.size() > MAX_LINES)
        {
            const size_t more = lines.size() - MAX_LINES;
            lines.resize(MAX_LINES);
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", (S32)more);
            lines.push_back(tr("GLTFExportDeniedMore", args));
        }
        LL_INFOS("GLTFExport") << "export denied:\n" << boost::algorithm::join(lines, "\n") << LL_ENDL;
        LLSD args;
        args["REASONS"] = boost::algorithm::join(lines, "\n");
        LLNotificationsUtil::add("GLTFExportDenied", args);
        return false;
    }
    return true;
}

void ExportJob::startPicker()
{
    std::string name = LLDir::getScrubbedFileName(mRootName);
    if (name.empty()) name = "object";
    const U32 serial = mSerial;
    mState = ST_PICKING;
    LLFilePickerReplyThread::startPicker(
        [serial](const std::vector<std::string>& filenames, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (LLAppViewer::instance()->quitRequested()) return;
            if (sJob && sJob->serial() == serial)
            {
                if (!filenames.empty()) sJob->onFilePicked(filenames[0]);
                else sJob->onPickCancelled();
            }
        },
        LLFilePicker::FFSAVE_GLB, name + ".glb",
        [serial](const std::vector<std::string>&, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (sJob && sJob->serial() == serial) sJob->onPickCancelled();
        });
}

void ExportJob::onFilePicked(const std::string& path)
{
    mPath = path;
    // A name typed without an extension is written as binary glTF at exactly the
    // chosen path (a sandbox's file portal grants only that path).
    startFetch();
}

void ExportJob::startFetch()
{
    mState = ST_FETCH;
    mStallTimer.reset();

    // Every texture the export will embed (and sculpt maps for regeneration).
    std::set<LLUUID> ids;
    for (PrimInfo& p : mPrims)
    {
        for (const AssetRef& a : p.assets)
        {
            if (!is_texture_kind(a.kind) || a.kind == KIND_SPECULAR) continue; // specular maps are not exported
            if (a.id.isNull() || a.id == IMG_WHITE || a.id == IMG_INVISIBLE
                || a.id == BLANK_OBJECT_NORMAL || a.id == DEFAULT_OBJECT_NORMAL || a.id == DEFAULT_OBJECT_SPECULAR)
            {
                continue;
            }
            ids.insert(a.id);
        }
    }
    for (const LLUUID& id : ids)
    {
        TextureFetch& t = mTextures[id];
        t.tex = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, true, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
        if (t.tex.isNull())
        {
            t.failed = true;
            continue;
        }
        t.old_boost = t.tex->getBoostLevel();
        t.tex->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
        t.tex->forceToSaveRawImage(0);
        t.tex->setLoadedCallback(loaded_callback, 0, true, false, new CallbackData{ mSerial, id }, &mCallbackList);
    }
    mAssetsTotal = (S32)mTextures.size();
    for (const PrimInfo& p : mPrims)
    {
        if (p.geom == GEOM_MESH) ++mAssetsTotal;
    }
}

void ExportJob::onTexture(const LLUUID& id, bool success, LLImageRaw* src, bool final)
{
    auto it = mTextures.find(id);
    if (it == mTextures.end() || it->second.done) return;
    TextureFetch& t = it->second;
    if (!success)
    {
        t.failed = true;
        t.done = true;
        return;
    }
    if (final && src && src->getData())
    {
        LLImageDataSharedLock lock(src);
        t.raw = new LLImageRaw((const U8*)src->getData(), src->getWidth(), src->getHeight(), src->getComponents());
        t.done = true;
        mStallTimer.reset();
        // Delivered below the asset's full size (a resolution cap): say so in the
        // completion notice rather than embedding a smaller image silently.
        LLViewerFetchedTexture* tex = t.tex;
        if (tex && tex->getFullWidth() > 0 && tex->getFullHeight() > 0
            && (src->getWidth() < tex->getFullWidth() || src->getHeight() < tex->getFullHeight()))
        {
            mReduced.push_back(llformat("%s (%dx%d of %dx%d)", id.asString().c_str(), src->getWidth(), src->getHeight(),
                                        tex->getFullWidth(), tex->getFullHeight()));
        }
    }
}

// Mesh: highest LOD the asset has, decoded into a private volume from the
// renderer's retained raw LOD bytes, else from LL's shared volume while its
// arrays are present, else requested from the mesh repository and polled.
bool ExportJob::tickMesh(PrimInfo& p)
{
    LLVOVolume* vobj = (LLVOVolume*)p.obj.get();
    const LLUUID& mesh_id = p.sculpt_id;
    if (p.bind_needed && !p.has_bind)
    {
        // Skinned in-world: the viewer draws joints * bind_shape * p and ignores the
        // prim transform, so the export uses the bind-shape (T-pose) positions.
        if (const LLMeshSkinInfo* skin = gMeshRepo.getSkinInfo(mesh_id, vobj))
        {
            memcpy(p.bind_shape, skin->mBindShapeMatrix.getF32ptr(), sizeof(p.bind_shape));
            p.has_bind = true;
        }
    }
    if (p.volume.notNull()) return !p.bind_needed || p.has_bind;
    const F32 now = LLFrameTimer::getElapsedSeconds();

    if (!gMeshRepo.hasHeader(mesh_id))
    {
        if (!p.mesh_requested || now - p.mesh_last_request > MESH_REREQUEST_SECS)
        {
            gMeshRepo.loadMesh(vobj, p.params, 3);
            p.mesh_requested = true;
            p.mesh_last_request = (F32)now;
        }
        return false;
    }
    if (p.mesh_lod < 0)
    {
        p.mesh_lod = gMeshRepo.getActualMeshLOD(p.params, 3);
        if (p.mesh_lod < 0)
        {
            p.mesh_failed = true; // header says no usable LOD (404 / unsupported version)
            return false;
        }
        p.mesh_requested = false;
    }

    const F32 detail = LLVolumeLODGroup::getVolumeScaleFromDetail(p.mesh_lod);
    std::vector<U8> bytes;
    if (LLCleanMeshAsset::instance().getRawMesh(mesh_id, p.mesh_lod, bytes) && !bytes.empty())
    {
        LLPointer<LLVolume> vol = new LLVolume(p.params, detail);
        if (vol->unpackVolumeFaces(bytes.data(), (S32)bytes.size()) && vol->getNumVolumeFaces() > 0)
        {
            p.volume = vol;
            mStallTimer.reset();
            return !p.bind_needed || p.has_bind;
        }
    }

    LLVolume* shared = LLPrimitive::getVolumeManager()->refVolume(p.params, p.mesh_lod);
    if (shared)
    {
        if (shared->isMeshAssetLoaded() && !shared->hasReleasedFaceGeometry() && shared->getNumVolumeFaces() > 0)
        {
            LLPointer<LLVolume> vol = new LLVolume(p.params, detail);
            vol->copyVolumeFaces(shared);
            p.volume = vol;
            mStallTimer.reset();
        }
        LLPrimitive::getVolumeManager()->unrefVolume(shared);
        if (p.volume.notNull()) return !p.bind_needed || p.has_bind;
    }

    if (!p.mesh_requested || now - p.mesh_last_request > MESH_REREQUEST_SECS)
    {
        gMeshRepo.loadMesh(vobj, p.params, p.mesh_lod);
        p.mesh_requested = true;
        p.mesh_last_request = (F32)now;
    }
    return false;
}

bool ExportJob::buildSculpt(PrimInfo& p)
{
    auto it = mTextures.find(p.sculpt_id);
    LLPointer<LLVolume> vol = new LLVolume(p.params, LLVolumeLODGroup::getVolumeScaleFromDetail(3));
    if (it != mTextures.end() && it->second.raw.notNull())
    {
        LLImageRaw* raw = it->second.raw;
        vol->sculpt(raw->getWidth(), raw->getHeight(), raw->getComponents(), raw->getData(), 0, false);
    }
    else
    {
        return false;
    }
    p.volume = vol;
    return vol->getNumVolumeFaces() > 0;
}

bool ExportJob::tickFetch()
{
    if (!objectsAlive())
    {
        fail(tr("GLTFExportFailGone"));
        return true;
    }
    for (auto& [id, t] : mTextures)
    {
        if (t.failed || (t.tex.notNull() && t.tex->isMissingAsset()))
        {
            LLStringUtil::format_map_t args;
            args["[ASSET]"] = id.asString();
            fail(tr("GLTFExportFailTexture", args));
            return true;
        }
    }
    bool pending = false;
    for (PrimInfo& p : mPrims)
    {
        if (p.geom == GEOM_MESH && !tickMesh(p)) pending = true;
        if (p.mesh_failed)
        {
            LLStringUtil::format_map_t args;
            args["[PRIM]"] = p.name;
            fail(tr("GLTFExportFailMesh", args));
            return true;
        }
    }
    for (auto& [id, t] : mTextures)
    {
        if (!t.done) pending = true;
    }
    if (pending)
    {
        if (mStallTimer.getElapsedTimeF32() > ASSET_STALL_TIMEOUT)
        {
            std::vector<std::string> items;
            for (auto& [id, t] : mTextures)
            {
                if (!t.done) items.push_back(id.asString());
            }
            for (PrimInfo& p : mPrims)
            {
                if (p.geom == GEOM_MESH && p.volume.isNull())
                {
                    LLStringUtil::format_map_t args;
                    args["[PRIM]"] = p.name;
                    items.push_back(tr("GLTFExportPendingMesh", args));
                }
            }
            LLStringUtil::format_map_t args;
            args["[ITEMS]"] = boost::algorithm::join(items, ", ");
            fail(tr("GLTFExportFailTimeout", args));
            return true;
        }
        return false;
    }

    // Everything is in: generate prim and sculpt volumes now.
    for (PrimInfo& p : mPrims)
    {
        if (p.geom == GEOM_PRIM)
        {
            p.volume = new LLVolume(p.params, LLVolumeLODGroup::getVolumeScaleFromDetail(3));
        }
        else if (p.geom == GEOM_SCULPT && !buildSculpt(p))
        {
            LLStringUtil::format_map_t args;
            args["[ASSET]"] = p.sculpt_id.asString();
            fail(tr("GLTFExportFailTexture", args));
            return true;
        }
    }
    mState = ST_WRITE;
    return false;
}

// ---------------------------------------------------------------------------
// Scene building
// ---------------------------------------------------------------------------

// LL's planar texgen (llface.cpp planarProjection), on the scaled volume position.
void planar_projection(LLVector2& tc, const LLVector4a& normal, const LLVector4a& vec)
{
    LLVector4a binormal;
    const F32 d = normal[0];
    if (d >= 0.5f || d <= -0.5f)
    {
        binormal.set(0.f, d < 0.f ? -1.f : 1.f, 0.f);
    }
    else
    {
        binormal.set(normal[1] > 0.f ? -1.f : 1.f, 0.f, 0.f);
    }
    LLVector4a tangent;
    tangent.setCross3(binormal, normal);
    tc.mV[1] = -((tangent.dot3(vec).getF32()) * 2.f - 0.5f);
    tc.mV[0] = 1.0f + ((binormal.dot3(vec).getF32()) * 2.f - 0.5f);
}

// LL's legacy (Blinn-Phong) texture transform, llface.cpp xform(): about the face
// centre rotate, then scale, then offset -- in SL UV space (v up).
LLVector2 sl_xform(const LLVector2& tc, F32 rot, F32 os, F32 ot, F32 ms, F32 mt)
{
    const F32 c = cosf(rot), s = sinf(rot);
    F32 u = tc.mV[0] - 0.5f, v = tc.mV[1] - 0.5f;
    const F32 tmp = u;
    u = u * c + v * s;
    v = -tmp * s + v * c;
    return LLVector2(u * ms + os + 0.5f, v * mt + ot + 0.5f);
}

// The legacy transform as an affine map in glTF UV space (v' = 1 - v):
// g' = L * g + t, row-major L = {a, b; c, d}.
struct Affine2 { F32 a, b, c, d, tx, ty; };

Affine2 legacy_affine_gltf(F32 rot, F32 os, F32 ot, F32 ms, F32 mt)
{
    auto map = [&](F32 gu, F32 gv)
    {
        LLVector2 sl = sl_xform(LLVector2(gu, 1.f - gv), rot, os, ot, ms, mt);
        return LLVector2(sl.mV[0], 1.f - sl.mV[1]);
    };
    const LLVector2 o = map(0.f, 0.f), x = map(1.f, 0.f), y = map(0.f, 1.f);
    return { x.mV[0] - o.mV[0], y.mV[0] - o.mV[0], x.mV[1] - o.mV[1], y.mV[1] - o.mV[1], o.mV[0], o.mV[1] };
}

// KHR_texture_transform is uv' = offset + R(rotation) * S * uv (R counter-clockwise,
// [c -s; s c]). The legacy map is S * R (scale after rotation), which only has that
// form when its columns are orthogonal (no rotation, a multiple of 90 degrees, or
// equal repeats). Returns false when it cannot be expressed exactly.
bool affine_to_khr(const Affine2& m, gx::TextureRef& ref)
{
    const F32 sx = sqrtf(m.a * m.a + m.c * m.c);
    if (sx < 1e-8f) return false;
    const F32 rot = atan2f(m.c, m.a);
    const F32 cr = cosf(rot), sr = sinf(rot);
    const F32 sy = -sr * m.b + cr * m.d;
    // reconstruct and compare
    const F32 eb = -sr * sy - m.b, ed = cr * sy - m.d;
    const F32 tol = 1e-4f * (1.f + fabsf(sx) + fabsf(sy));
    if (fabsf(eb) > tol || fabsf(ed) > tol) return false;
    ref.offset[0] = m.tx;
    ref.offset[1] = m.ty;
    ref.rotation = rot;
    ref.scale[0] = sx;
    ref.scale[1] = sy;
    ref.has_transform = !(fabsf(m.tx) < 1e-6f && fabsf(m.ty) < 1e-6f && fabsf(rot) < 1e-6f
                          && fabsf(sx - 1.f) < 1e-6f && fabsf(sy - 1.f) < 1e-6f);
    return true;
}

struct ImageCache
{
    gx::Scene* scene = nullptr;
    std::map<LLUUID, TextureFetch>* textures = nullptr;
    std::map<LLUUID, S32> index;          // texture id -> scene image (-1 = unusable)
    std::map<LLUUID, bool> has_alpha;     // any pixel with alpha < 255
    std::string error;

    S32 get(const LLUUID& id)
    {
        auto it = index.find(id);
        if (it != index.end()) return it->second;
        S32 result = -1;
        auto tex = textures->find(id);
        if (tex != textures->end() && tex->second.raw.notNull())
        {
            LLPointer<LLImageRaw> raw = tex->second.raw;
            bool alpha = false;
            if (raw->getComponents() == 4)
            {
                const U8* d = raw->getData();
                const S32 n = raw->getWidth() * raw->getHeight();
                for (S32 i = 0; i < n && !alpha; ++i) alpha = d[i * 4 + 3] != 255;
                if (!alpha)
                {
                    // Opaque: drop the alpha channel (smaller file, no false transparency).
                    LLPointer<LLImageRaw> rgb = new LLImageRaw(raw->getWidth(), raw->getHeight(), 3);
                    rgb->copyUnscaled4onto3(raw);
                    raw = rgb;
                }
            }
            else if (raw->getComponents() == 2)
            {
                const U8* d = raw->getData();
                const S32 n = raw->getWidth() * raw->getHeight();
                for (S32 i = 0; i < n && !alpha; ++i) alpha = d[i * 2 + 1] != 255;
            }
            has_alpha[id] = alpha;
            LLPointer<LLImagePNG> png = new LLImagePNG();
            if (png->encode(raw, 0.f) && png->getDataSize() > 0)
            {
                gx::Image img;
                img.name = id.asString();
                img.png.assign(png->getData(), png->getData() + png->getDataSize());
                scene->images.push_back(std::move(img));
                result = (S32)scene->images.size() - 1;
            }
            else
            {
                error = id.asString();
            }
        }
        index[id] = result;
        return result;
    }
};

// Per-face output: texcoord sets (set 0 = base UVs in glTF space) + extra baked sets.
struct FaceUV
{
    std::vector<LLVector2> base;            // glTF space
    std::vector<std::vector<float> > sets;  // written to the primitive
    std::vector<Affine2> baked;             // transform of each extra set (sets[1..])

    S32 setFor(const Affine2& m)
    {
        for (size_t i = 0; i < baked.size(); ++i)
        {
            const Affine2& b = baked[i];
            if (b.a == m.a && b.b == m.b && b.c == m.c && b.d == m.d && b.tx == m.tx && b.ty == m.ty) return (S32)i + 1;
        }
        std::vector<float> uv;
        uv.reserve(base.size() * 2);
        for (const LLVector2& g : base)
        {
            uv.push_back(m.a * g.mV[0] + m.b * g.mV[1] + m.tx);
            uv.push_back(m.c * g.mV[0] + m.d * g.mV[1] + m.ty);
        }
        sets.push_back(std::move(uv));
        baked.push_back(m);
        return (S32)baked.size();
    }

    // Texture slot with a legacy transform: KHR_texture_transform when exact, else a baked set.
    void assign(gx::TextureRef& ref, const Affine2& m)
    {
        if (affine_to_khr(m, ref))
        {
            ref.tex_coord = 0;
        }
        else
        {
            ref.tex_coord = setFor(m);
            ref.has_transform = false;
        }
    }
};

void copy_transform(gx::TextureRef& ref, const LLGLTFMaterial::TextureTransform& t)
{
    ref.offset[0] = t.mOffset.mV[VX];
    ref.offset[1] = t.mOffset.mV[VY];
    ref.rotation = t.mRotation;
    ref.scale[0] = t.mScale.mV[VX];
    ref.scale[1] = t.mScale.mV[VY];
    ref.has_transform = !(t.mOffset.isExactlyZero() && t.mRotation == 0.f
                          && t.mScale.mV[VX] == 1.f && t.mScale.mV[VY] == 1.f);
    ref.tex_coord = 0;
}

gx::Material pbr_material(const LLGLTFMaterial& m, ImageCache& images, const LLUUID& mat_id)
{
    gx::Material out;
    out.name = mat_id.asString();
    for (int i = 0; i < 4; ++i) out.base_color[i] = m.mBaseColor.mV[i];
    out.metallic = m.mMetallicFactor;
    out.roughness = m.mRoughnessFactor;
    for (int i = 0; i < 3; ++i) out.emissive[i] = m.mEmissiveColor.mV[i];
    out.alpha_mode = m.getAlphaMode();
    out.alpha_cutoff = m.mAlphaCutoff;
    out.double_sided = m.mDoubleSided;
    auto slot = [&](gx::TextureRef& ref, U32 info)
    {
        if (m.mTextureId[info].isNull()) return;
        ref.image = images.get(m.mTextureId[info]);
        if (ref.image >= 0) copy_transform(ref, m.mTextureTransform[info]);
    };
    slot(out.base_color_tex, LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR);
    slot(out.normal_tex, LLGLTFMaterial::GLTF_TEXTURE_INFO_NORMAL);
    slot(out.metallic_roughness_tex, LLGLTFMaterial::GLTF_TEXTURE_INFO_METALLIC_ROUGHNESS);
    // LL materials are ORM: occlusion lives in the red channel of the same texture.
    out.occlusion_tex = out.metallic_roughness_tex;
    slot(out.emissive_tex, LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE);
    out.extras.push_back({ "sl_material", "pbr" });
    out.extras.push_back({ "sl_material_id", mat_id.asString() });
    return out;
}

gx::Material legacy_material(const LLTextureEntry* te, ImageCache& images, FaceUV& uv)
{
    gx::Material out;
    out.name = "blinn-phong";
    out.metallic = 0.f;
    const LLColor4& color = te->getColor();
    for (int i = 0; i < 4; ++i) out.base_color[i] = color.mV[i];
    const LLMaterial* lm = te->getMaterialParams().get();

    const LLUUID& diffuse = te->getID();
    const bool invisible = diffuse == IMG_INVISIBLE;
    bool tex_alpha = false;
    if (diffuse.notNull() && diffuse != IMG_WHITE && !invisible)
    {
        out.base_color_tex.image = images.get(diffuse);
        if (out.base_color_tex.image >= 0)
        {
            tex_alpha = images.has_alpha[diffuse];
            uv.assign(out.base_color_tex, legacy_affine_gltf(te->getRotation(), te->getOffsetS(), te->getOffsetT(),
                                                             te->getScaleS(), te->getScaleT()));
        }
    }

    // Alpha: the material's diffuse alpha mode when it has one, else blend for a
    // texture with transparency or a transparent colour (as the viewer sorts it).
    if (invisible)
    {
        out.alpha_mode = "BLEND";
        out.base_color[3] = 0.f;
    }
    else if (lm && lm->getDiffuseAlphaMode() != LLMaterial::DIFFUSE_ALPHA_MODE_DEFAULT)
    {
        switch (lm->getDiffuseAlphaMode())
        {
        case LLMaterial::DIFFUSE_ALPHA_MODE_BLEND: out.alpha_mode = "BLEND"; break;
        case LLMaterial::DIFFUSE_ALPHA_MODE_MASK:
            out.alpha_mode = "MASK";
            out.alpha_cutoff = lm->getAlphaMaskCutoff() / 255.f;
            break;
        default: out.alpha_mode = "OPAQUE"; break; // NONE, EMISSIVE (mask not exported)
        }
        if (color.mV[VALPHA] < 0.999f) out.alpha_mode = "BLEND";
    }
    else
    {
        out.alpha_mode = (tex_alpha || color.mV[VALPHA] < 0.999f) ? "BLEND" : "OPAQUE";
    }

    // Specular approximation: roughness from glossiness, metallic 0.
    if (lm)
    {
        out.roughness = llclamp(1.f - lm->getSpecularLightExponent() / 255.f, 0.f, 1.f);
        const LLUUID& normal = lm->getNormalID();
        if (normal.notNull() && normal != BLANK_OBJECT_NORMAL && normal != DEFAULT_OBJECT_NORMAL)
        {
            out.normal_tex.image = images.get(normal);
            if (out.normal_tex.image >= 0)
            {
                F32 os, ot, ms, mt;
                lm->getNormalOffset(os, ot);
                lm->getNormalRepeat(ms, mt);
                uv.assign(out.normal_tex, legacy_affine_gltf(lm->getNormalRotation(), os, ot, ms, mt));
            }
        }
    }
    else
    {
        static const F32 shiny_roughness[4] = { 1.f, 0.75f, 0.5f, 0.25f };
        out.roughness = shiny_roughness[te->getShiny() & 3];
    }

    // Fullbright / glow -> emissive (the diffuse texture modulated by the colour).
    const F32 glow = llclamp(te->getGlow(), 0.f, 1.f);
    const F32 emit = te->getFullbright() ? 1.f : glow;
    if (emit > 0.f && !invisible)
    {
        for (int i = 0; i < 3; ++i) out.emissive[i] = color.mV[i] * emit;
        out.emissive_tex = out.base_color_tex;
    }
    out.extras.push_back({ "sl_material", "blinn-phong" });
    if (te->getFullbright()) out.extras.push_back({ "sl_fullbright", "1" });
    return out;
}

gx::Primitive face_primitive(PrimInfo& p, S32 f, gx::Scene& scene, ImageCache& images,
                                       const std::map<LLUUID, LLPointer<LLFetchedGLTFMaterial> >& mats)
{
    gx::Primitive prim;
    const LLVolumeFace& vf = p.volume->getVolumeFace(f);
    if (!vf.mPositions || !vf.mIndices || vf.mNumVertices <= 0 || vf.mNumIndices < 3)
    {
        return prim;
    }
    LLViewerObject* obj = p.obj;
    const S32 te_idx = llmin(f, (S32)obj->getNumTEs() - 1);
    const LLTextureEntry* te = obj->getTE((U8)llmax(te_idx, 0));

    const S32 nv = vf.mNumVertices;
    prim.positions.reserve(nv * 3);
    prim.normals.reserve(nv * 3);
    LLMatrix4a bind;
    LLMatrix3 bind_normal;
    if (p.has_bind)
    {
        bind.loadu(p.bind_shape);
        // Row-vector convention (v * M): normals take the inverse transpose.
        const F32* m = p.bind_shape;
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) bind_normal.mMatrix[r][c] = m[r * 4 + c];
        bind_normal.invert();
        bind_normal.transpose();
    }
    for (S32 i = 0; i < nv; ++i)
    {
        LLVector4a pv = vf.mPositions[i];
        if (p.has_bind)
        {
            LLVector4a t;
            bind.affineTransform(pv, t);
            pv = t;
        }
        const F32* pos = pv.getF32ptr();
        prim.positions.insert(prim.positions.end(), { pos[0], pos[1], pos[2] });
        if (vf.mNormals)
        {
            LLVector3 n(vf.mNormals[i].getF32ptr());
            if (p.has_bind) n = n * bind_normal;
            if (n.normVec() < 1e-6f) n.set(0.f, 0.f, 1.f);
            prim.normals.insert(prim.normals.end(), { n.mV[0], n.mV[1], n.mV[2] });
        }
    }
    if (!vf.mNormals) prim.normals.clear();
    for (S32 i = 0; i + 2 < vf.mNumIndices; i += 3)
    {
        prim.indices.insert(prim.indices.end(), { (U32)vf.mIndices[i], (U32)vf.mIndices[i + 1], (U32)vf.mIndices[i + 2] });
    }

    // Base UVs: planar texgen when the face uses it, then flipped into glTF space.
    FaceUV uv;
    uv.base.resize(nv);
    const bool planar = te && te->getTexGen() == LLTextureEntry::TEX_GEN_PLANAR && vf.mNormals;
    LLVector4a scale;
    scale.load3(obj->getScale().mV);
    for (S32 i = 0; i < nv; ++i)
    {
        LLVector2 tc = vf.mTexCoords ? vf.mTexCoords[i] : LLVector2(0.f, 0.f);
        if (planar)
        {
            LLVector4a vec = vf.mPositions[i];
            vec.mul(scale);
            LLVector4a nrm = vf.mNormals[i];
            nrm.normalize3fast();
            planar_projection(tc, nrm, vec);
        }
        uv.base[i].set(tc.mV[0], 1.f - tc.mV[1]);
    }
    std::vector<float> base;
    base.reserve(nv * 2);
    for (const LLVector2& g : uv.base) base.insert(base.end(), { g.mV[0], g.mV[1] });
    uv.sets.push_back(std::move(base));

    gx::Material mat;
    LLGLTFMaterial pbr;
    if (te && render_material(obj, te_idx, mats, pbr))
    {
        mat = pbr_material(pbr, images, obj->getRenderMaterialID((U8)te_idx));
    }
    else if (te)
    {
        mat = legacy_material(te, images, uv);
    }
    prim.material = gx::addMaterialDedup(scene, mat);
    prim.uvs = std::move(uv.sets);
    return prim;
}

bool ExportJob::writeFile()
{
    if (!objectsAlive())
    {
        fail(tr("GLTFExportFailGone"));
        return true;
    }
    gx::Scene scene;
    scene.name = mRootName.empty() ? std::string("Second Life object") : mRootName;
    scene.generator = "Slipstream Viewer glTF export";
    ImageCache images;
    images.scene = &scene;
    images.textures = &mTextures;

    // Linkset placement: the primary root at the origin (or at its region
    // position when ExportGLTFRegionPosition is set), other linksets at their
    // offset from it. Orientation is kept.
    auto world = [](LLViewerObject* root, LLVector3& pos, LLQuaternion& rot)
    {
        if (root->isAttachment())
        {
            pos = root->getRenderPosition();
            rot = root->getRenderRotation();
        }
        else
        {
            pos = root->getPositionAgent();
            rot = root->getRotation();
        }
    };
    LLVector3 primary_pos;
    LLQuaternion primary_rot;
    world(mRoots.front(), primary_pos, primary_rot);
    LLVector3 origin;
    static LLCachedControl<bool> keep_region(gSavedSettings, "ExportGLTFRegionPosition", false);
    if (keep_region && mRoots.front()->getRegion())
    {
        origin = mRoots.front()->getRegion()->getPosRegionFromAgent(primary_pos);
    }

    S32 exported = 0;
    for (LLViewerObject* root : mRoots)
    {
        LLVector3 pos;
        LLQuaternion rot;
        world(root, pos, rot);
        gx::Node linkset;
        const LLVector3 t = origin + (pos - primary_pos);
        for (int i = 0; i < 3; ++i) linkset.translation[i] = t.mV[i];
        for (int i = 0; i < 4; ++i) linkset.rotation[i] = rot.mQ[i];
        const S32 linkset_index = (S32)scene.nodes.size();

        // Prims of this linkset in link order.
        std::vector<PrimInfo*> prims;
        for (PrimInfo& p : mPrims)
        {
            if (p.root == root) prims.push_back(&p);
        }
        std::sort(prims.begin(), prims.end(), [](const PrimInfo* a, const PrimInfo* b) { return a->link_number < b->link_number; });
        for (const PrimInfo* p : prims)
        {
            if (p->obj == root) linkset.name = p->name;
        }
        if (linkset.name.empty()) linkset.name = prims.empty() ? std::string("linkset") : prims.front()->name;
        linkset.extras_str.push_back({ "sl_kind", "linkset" });
        scene.nodes.push_back(linkset);

        for (PrimInfo* p : prims)
        {
            gx::Node node;
            node.parent = linkset_index;
            node.name = p->name;
            if (p->has_bind)
            {
                // Bind-pose geometry is already in avatar space: identity transform.
                node.extras_str.push_back({ "sl_rigged", "bind pose" });
            }
            else if (p->obj != root)
            {
                const LLVector3& lp = p->obj->getPosition();
                const LLQuaternion& lr = p->obj->getRotation();
                for (int i = 0; i < 3; ++i) node.translation[i] = lp.mV[i];
                for (int i = 0; i < 4; ++i) node.rotation[i] = lr.mQ[i];
            }
            if (!p->has_bind)
            {
                const LLVector3& s = p->obj->getScale();
                for (int i = 0; i < 3; ++i) node.scale[i] = s.mV[i];
            }
            node.extras_int.push_back({ "link_number", p->link_number });
            if (!p->desc.empty()) node.extras_str.push_back({ "description", p->desc });
            node.extras_str.push_back({ "sl_geometry", p->geom == GEOM_MESH ? "mesh" : p->geom == GEOM_SCULPT ? "sculpt" : "prim" });

            for (S32 f = 0; f < p->volume->getNumVolumeFaces(); ++f)
            {
                gx::Primitive prim = face_primitive(*p, f, scene, images, mMaterials);
                if (!prim.indices.empty()) node.primitives.push_back(std::move(prim));
            }
            if (!images.error.empty())
            {
                LLStringUtil::format_map_t args;
                args["[ASSET]"] = images.error;
                fail(tr("GLTFExportFailEncode", args));
                return true;
            }
            scene.nodes.push_back(std::move(node));
            ++exported;
        }
    }

    std::string error, note;
    if (!gx::writeScene(scene, mPath, error, &note))
    {
        LLStringUtil::format_map_t args;
        args["[ERROR]"] = error;
        fail(tr("GLTFExportFailWrite", args));
        return true;
    }
    LL_INFOS("GLTFExport") << "exported " << exported << " prims to " << mPath << LL_ENDL;
    LLSD args;
    args["COUNT"] = exported;
    args["PATH"] = mPath;
    std::string notes = note.empty() ? std::string() : tr("GLTFExportNoteEmbedded");
    if (!mReduced.empty())
    {
        LLStringUtil::format_map_t rargs;
        rargs["[TEXTURES]"] = boost::algorithm::join(mReduced, ", ");
        LL_WARNS("GLTFExport") << "textures below full resolution: " << boost::algorithm::join(mReduced, ", ") << LL_ENDL;
        if (!notes.empty()) notes += "\n";
        notes += tr("GLTFExportNoteReduced", rargs);
    }
    args["NOTE"] = notes;
    LLNotificationsUtil::add("GLTFExportDone", args);
    mState = ST_DONE;
    return true;
}

bool ExportJob::tick()
{
    switch (mState)
    {
    case ST_PROPERTIES: return tickProperties();
    case ST_MATERIALS:  return tickMaterials();
    case ST_INVENTORY:  return tickInventory();
    case ST_CHECK:
        if (!objectsAlive())
        {
            fail(tr("GLTFExportFailGone"));
            return true;
        }
        if (!checkPermissions())
        {
            mState = ST_DONE;
            return true;
        }
        mState = ST_PICK;
        return false;
    case ST_PICK:
        startPicker();
        return false;
    case ST_PICKING:
        return false;
    case ST_FETCH:
        return tickFetch();
    case ST_WRITE:
        return writeFile();
    case ST_DONE:
    default:
        return true;
    }
}

std::string ExportJob::statusText() const
{
    switch (mState)
    {
    case ST_PROPERTIES: return tr("GLTFExportStatusProperties");
    case ST_MATERIALS:  return tr("GLTFExportStatusMaterials");
    case ST_INVENTORY:  return tr("GLTFExportStatusInventory");
    case ST_CHECK:
    case ST_PICK:
    case ST_PICKING:    return tr("GLTFExportStatusPicking");
    case ST_FETCH:
    {
        LLStringUtil::format_map_t args;
        S32 done = 0;
        for (const auto& [id, t] : mTextures) if (t.done) ++done;
        for (const PrimInfo& p : mPrims) if (p.geom == GEOM_MESH && p.volume.notNull()) ++done;
        args["[DONE]"] = llformat("%d", done);
        args["[TOTAL]"] = llformat("%d", mAssetsTotal);
        return tr("GLTFExportStatusFetching", args);
    }
    case ST_WRITE:      return tr("GLTFExportStatusWriting");
    default:            return std::string();
    }
}

F32 ExportJob::progress() const
{
    if (mState != ST_FETCH) return mState >= ST_WRITE ? 1.f : 0.f;
    if (mAssetsTotal <= 0) return 1.f;
    S32 done = 0;
    for (const auto& [id, t] : mTextures) if (t.done) ++done;
    for (const PrimInfo& p : mPrims) if (p.geom == GEOM_MESH && p.volume.notNull()) ++done;
    return (F32)done / (F32)mAssetsTotal;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

// static
bool LLGLTFExporter::canExportSelection()
{
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    if (sel->isEmpty()) return false;
    bool any = false;
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLViewerObject* obj = (*it)->getObject();
        if (!obj) continue;
        if (obj->asAvatar() || obj->getPCode() != LL_PCODE_VOLUME) return false;
        if (obj->isAttachment() && obj->getAvatar() != gAgentAvatarp.get()) return false;
        LLViewerObject* root = obj->getRootEdit();
        if (!root || !root->permYouOwner()) return false;
        any = true;
    }
    return any;
}

// static
void LLGLTFExporter::exportSelection()
{
    if (sJob)
    {
        LLNotificationsUtil::add("GLTFExportBusy");
        return;
    }
    const U32 serial = ++sSerial;
    sJob = std::make_unique<ExportJob>(serial);
    if (!sJob->init())
    {
        sJob.reset();
        return;
    }
    LLFloaterReg::showInstance("gltf_export");
    doOnIdleRepeating([serial]()
    {
        if (!sJob || sJob->serial() != serial) return true;
        if (sJob->tick())
        {
            sJob.reset();
            if (LLFloater* f = LLFloaterReg::findInstance("gltf_export")) f->closeFloater();
            return true;
        }
        return false;
    });
}

// static
bool LLGLTFExporter::isActive()
{
    return (bool)sJob;
}

// static
void LLGLTFExporter::cancel()
{
    if (sJob)
    {
        LL_INFOS("GLTFExport") << "export cancelled" << LL_ENDL;
        sJob.reset();   // the idle callback sees the serial gone and stops
    }
}

// static
std::string LLGLTFExporter::getStatusText()
{
    return sJob ? sJob->statusText() : std::string();
}

// static
float LLGLTFExporter::getProgress()
{
    return sJob ? sJob->progress() : 0.f;
}
