/**
 * @file llviewertexture.h
 * @brief Object for managing images and their textures
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

#ifndef LL_LLVIEWERTEXTURE_H
#define LL_LLVIEWERTEXTURE_H

#include "llatomic.h"
#include "llgltexture.h"
#include "lltimer.h"
#include "llframetimer.h"
#include "llhost.h"
#include "llpixelformat.h"
#include "llrender.h"
#include "llmetricperformancetester.h"
#include "httpcommon.h"
#include "workqueue.h"
#include "gltf/common.h"

#include <map>
#include <list>

class LLFace;
class LLTextureData ;
class LLImageRaw;
class LLViewerTexture;
class LLViewerFetchedTexture ;
class LLViewerMediaTexture ;
class LLTexturePipelineTester ;


typedef void    (*loaded_callback_func)( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* src_aux, S32 discard_level, bool final, void* userdata );

class LLFileSystem;
class LLMessageSystem;
class LLViewerMediaImpl ;
class LLVOVolume ;
struct LLTextureKey;

class LLLoadedCallbackEntry
{
public:
    typedef std::set< LLTextureKey > source_callback_list_t;

public:
    LLLoadedCallbackEntry(loaded_callback_func cb,
                          S32 discard_level,
                          bool need_imageraw, // Needs image raw for the callback
                          void* userdata,
                          source_callback_list_t* src_callback_list,
                          LLViewerFetchedTexture* target,
                          bool pause);
    ~LLLoadedCallbackEntry();
    void removeTexture(LLViewerFetchedTexture* tex) ;

    loaded_callback_func    mCallback;
    S32                     mLastUsedDiscard;
    S32                     mDesiredDiscard;
    bool                    mNeedsImageRaw;
    bool                    mPaused;
    void*                   mUserData;
    source_callback_list_t* mSourceCallbackList;

public:
    static void cleanUpCallbackList(LLLoadedCallbackEntry::source_callback_list_t* callback_list) ;
};

class LLTextureBar;

class LLViewerTexture : public LLGLTexture
{
public:
    enum
    {
        LOCAL_TEXTURE,
        MEDIA_TEXTURE,
        DYNAMIC_TEXTURE,
        FETCHED_TEXTURE,
        LOD_TEXTURE,
        INVALID_TEXTURE_TYPE
    };

    typedef std::vector<class LLFace*> ll_face_list_t;
    typedef std::vector<LLVOVolume*> ll_volume_list_t;


protected:
    virtual ~LLViewerTexture();
    LOG_CLASS(LLViewerTexture);

public:
    static void initClass();
    static void updateClass();
    static bool isSystemMemoryLow();

    LLViewerTexture(bool usemipmaps = true);
    LLViewerTexture(const LLUUID& id, bool usemipmaps) ;
    LLViewerTexture(const LLImageRaw* raw, bool usemipmaps) ;
    LLViewerTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps) ;

    virtual S8 getType() const;
    virtual bool isMissingAsset() const ;
    virtual void dump();    // debug info to LL_INFOS()

    virtual bool isViewerMediaTexture() const { return false; }

    /*virtual*/ bool bindDefaultImage(const S32 stage = 0) ;
    /*virtual*/ bool bindDebugImage(const S32 stage = 0) ;
    /*virtual*/ void forceImmediateUpdate() ;
    /*virtual*/ bool isActiveFetching();

    /*virtual*/ const LLUUID& getID() const { return mID; }
    virtual void setBoostLevel(S32 level);
    S32  getBoostLevel() { return mBoostLevel; }
    void setTextureListType(S32 tex_type) { mTextureListType = tex_type; }
    S32 getTextureListType() { return mTextureListType; }

    void addTextureStats(F32 virtual_size, bool needs_gltexture = true) const;
    void resetTextureStats();
    void setMaxVirtualSizeResetInterval(S32 interval)const {mMaxVirtualSizeResetInterval = interval;}
    void resetMaxVirtualSizeResetCounter()const {mMaxVirtualSizeResetCounter = mMaxVirtualSizeResetInterval;}
    S32 getMaxVirtualSizeResetCounter() const { return mMaxVirtualSizeResetCounter; }

    virtual F32  getMaxVirtualSize() ;

    LLFrameTimer* getLastReferencedTimer() { return &mLastReferencedTimer; }

    S32 getFullWidth() const { return mFullWidth; }
    S32 getFullHeight() const { return mFullHeight; }
    /*virtual*/ void setKnownDrawSize(S32 width, S32 height);

    virtual void addFace(U32 channel, LLFace* facep) ;
    virtual void removeFace(U32 channel, LLFace* facep) ;
    S32 getTotalNumFaces() const;
    S32 getNumFaces(U32 ch) const;
    const ll_face_list_t* getFaceList(U32 channel) const {llassert(channel < LLRender::NUM_TEXTURE_CHANNELS); return &mFaceList[channel];}

    virtual void addVolume(U32 channel, LLVOVolume* volumep);
    virtual void removeVolume(U32 channel, LLVOVolume* volumep);
    S32 getNumVolumes(U32 channel) const;
    const ll_volume_list_t* getVolumeList(U32 channel) const { return &mVolumeList[channel]; }

    bool isLargeImage() ;

    void setParcelMedia(LLViewerMediaTexture* media) {mParcelMedia = media;}
    bool hasParcelMedia() const { return mParcelMedia != NULL;}
    LLViewerMediaTexture* getParcelMedia() const { return mParcelMedia;}

    /*virtual*/ void updateBindStatsForTester() ;

    struct MaterialEntry
    {
        S32 mIndex = LL::GLTF::INVALID_INDEX;
        std::shared_ptr<LL::GLTF::Asset> mAsset;
    };
    typedef std::vector<MaterialEntry> material_list_t;
    material_list_t   mMaterialList;  // reverse pointer pointing to LL::GLTF::Materials using this image as texture

protected:
    void cleanup() ;
    void init(bool firstinit) ;
    void reorganizeFaceList() ;
    void reorganizeVolumeList();

private:
    friend class LLUIImageList;

    static U32Megabytes getFreeSystemMemory();

protected:
    friend class LLViewerTextureList;
    LLUUID mID;
    S32 mTextureListType; // along with mID identifies where to search for this texture in TextureList

    mutable F32 mMaxVirtualSize = 0.f;  // The largest virtual size of the image, in pixels - how much data to we need?
    mutable S32  mMaxVirtualSizeResetCounter;
    mutable S32  mMaxVirtualSizeResetInterval;
    LLFrameTimer mLastReferencedTimer;

    ll_face_list_t    mFaceList[LLRender::NUM_TEXTURE_CHANNELS]; //reverse pointer pointing to the faces using this image as texture
    U32               mNumFaces[LLRender::NUM_TEXTURE_CHANNELS];
    LLFrameTimer      mLastFaceListUpdateTimer ;

    ll_volume_list_t  mVolumeList[LLRender::NUM_VOLUME_TEXTURE_CHANNELS];
    U32                 mNumVolumes[LLRender::NUM_VOLUME_TEXTURE_CHANNELS];
    LLFrameTimer      mLastVolumeListUpdateTimer;

    //do not use LLPointer here.
    LLViewerMediaTexture* mParcelMedia ;


public:
    static const U32 sCurrentFileVersion;
    static S32 sImageCount;
    static S32 sRawCount;
    static S32 sAuxCount;
    static F32 sDesiredDiscardBias;   // pinned at 1 (no VRAM-pressure ramp)
    static S32 sMaxSculptRez ;
    static U32 sMinLargeImageSize ;
    static U32 sMaxSmallImageSize ;
    static bool sFreezeImageUpdates;
    static F32  sCurrentTime ;

    enum EDebugTexels
    {
        DEBUG_TEXELS_OFF,
        DEBUG_TEXELS_CURRENT,
        DEBUG_TEXELS_DESIRED,
        DEBUG_TEXELS_FULL
    };

    static EDebugTexels sDebugTexelsMode;

    static LLPointer<LLViewerTexture> sNullImagep; // Null texture for non-textured objects.
    static LLPointer<LLViewerTexture> sBlackImagep; // Texture to show NOTHING (pure black)
    static LLPointer<LLViewerTexture> sCheckerBoardImagep;  // Texture to show NOTHING (pure black)
};


enum FTType
{
    FTT_UNKNOWN = -1,
    FTT_DEFAULT = 0, // standard texture fetched by id.
    FTT_SERVER_BAKE, // texture produced by appearance service and fetched from there.
    FTT_HOST_BAKE, // old-style baked texture uploaded by viewer and fetched from avatar's host.
    FTT_MAP_TILE, // tiles are fetched from map server directly.
    FTT_LOCAL_FILE // fetch directly from a local file.
};

const std::string& fttype_to_string(const FTType& fttype);

//
//textures are managed in gTextureList.
//raw image data is fetched from remote or local cache
//but the raw image this texture pointing to is fixed.
//
class LLViewerFetchedTexture : public LLViewerTexture
{
    friend class LLTextureBar; // debug info only
    friend class LLTextureView; // debug info only

protected:
    /*virtual*/ ~LLViewerFetchedTexture();
public:
    LLViewerFetchedTexture(const LLUUID& id, FTType f_type, const LLHost& host = LLHost(), bool usemipmaps = true);
    LLViewerFetchedTexture(const LLImageRaw* raw, FTType f_type, bool usemipmaps);
    LLViewerFetchedTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps = true);

public:

    struct Compare
    {
        // lhs < rhs
        bool operator()(const LLPointer<LLViewerFetchedTexture> &lhs, const LLPointer<LLViewerFetchedTexture> &rhs) const
        {
            const LLViewerFetchedTexture* lhsp = (const LLViewerFetchedTexture*)lhs;
            const LLViewerFetchedTexture* rhsp = (const LLViewerFetchedTexture*)rhs;

            // greater priority is "less"
            const F32 lpriority = lhsp->mMaxVirtualSize;
            const F32 rpriority = rhsp->mMaxVirtualSize;
            if (lpriority > rpriority) // higher priority
                return true;
            if (lpriority < rpriority)
                return false;
            return lhsp < rhsp;
        }
    };

public:
    /*virtual*/ S8 getType() const override;
    FTType getFTType() const;
    /*virtual*/ void forceImmediateUpdate() override;
    /*virtual*/ void dump() override;

    // Set callbacks to get called when the image gets updated with higher
    // resolution versions.
    void setLoadedCallback(loaded_callback_func cb,
                           S32 discard_level, bool keep_imageraw, bool needs_aux,
                           void* userdata, LLLoadedCallbackEntry::source_callback_list_t* src_callback_list, bool pause = false);
    bool hasCallbacks() { return !mLoadedCallbackList.empty(); }
    void pauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    void unpauseLoadedCallbacks(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    bool doLoadedCallbacks();
    void deleteCallbackEntry(const LLLoadedCallbackEntry::source_callback_list_t* callback_list);
    void clearCallbackEntryList() ;

    // #309: LLViewerTextureList visited every texture with a load callback every frame
    // (for up to 900 s each). doLoadedCallbacks()'s outcome depends only on the state in
    // LoadedCallbackState plus two timers, so it is skipped while that state is unchanged
    // since it last ran, and re-run at least every LOADED_CALLBACK_RECHECK_SECS for the
    // timers (900 s inactivity drop, 5 s forced refetch).
    bool loadedCallbacksNeedUpdate() const;
    void noteLoadedCallbacksRan();          // call after doLoadedCallbacks()
    static constexpr F32 LOADED_CALLBACK_RECHECK_SECS = 1.f;

    void addToCreateTexture();

    //call to determine if createTexture is necessary
    bool preCreateTexture(S32 usename = 0);
     // ONLY call from LLViewerTextureList or ImageGL background thread
    bool createTexture(S32 usename = 0);
    void postCreateTexture();
    void scheduleCreateTexture();

    void destroyTexture() ;

    virtual void processTextureStats() ;

    bool needsAux() const { return mNeedsAux; }

    // Host we think might have this image, used for baked av textures.
    void setTargetHost(LLHost host)         { mTargetHost = host; }
    LLHost getTargetHost() const            { return mTargetHost; }

    void updateVirtualSize() ;

    S32  getDesiredDiscardLevel()            { return mDesiredDiscardLevel; }
    void setMinDiscardLevel(S32 discard)    { mMinDesiredDiscardLevel = llmin(mMinDesiredDiscardLevel,(S8)discard); }

    void setBoostLevel(S32 level) override;
    bool updateFetch();

    // VULKAN TEXTURE RESIDENCY. The clean renderer's per-texture on-screen demand: the
    // top-mip size (texels) the world needs this pass, already floored and mip-biased;
    // VK_DEMAND_FULL = full resolution. While fresh it replaces the face-area virtual
    // size for BOOST_NONE textures (applyVKDemand): the decode follows what is on
    // screen, a retained CPU copy sharper than that is scaled down off-thread, and the
    // full J2C stays in the disk cache so sharpening re-decodes locally. Max over the
    // callers of one pass.
    static constexpr U32 VK_DEMAND_FULL = 0x00FFFFFFu;
    // gpu_texels: the top-mip size the renderer's GPU image holds right now (0 = none),
    // or a usable GPU-ready disk cache chain will give it (tier 2). When it covers the
    // demand, no sharper decode is needed and the CPU copy has no world reader (#707: it
    // is dropped, see vkCpuCopyStep). t2_safe: the GPU image can come back without a
    // decode-from-network -- its tier-2 capture exists, or it is never captured (tier-2
    // ineligible: J2C re-source) -- so the copy may go.
    void setVKDemand(U32 texels, U32 pass, U32 gpu_texels = 0, bool t2_safe = false,
                     bool needs_decode = false, bool mask_variant = false);
    // Diagnostics (VKMem log): textures whose decode the VK demand drives, how many of
    // those decode below full, how many are waiting on a sharper decode, and the tier-1
    // (CPU copy) leased / unleased split; plus CPU copies scaled down / dropped so far.
    // #535: SLICED -- each call walks the texture list for up to budget_s (0 = no
    // budget) and returns true, filling `out`, when a full pass completed.
    struct VKTexStats { U32 driven = 0, reduced = 0, sharpen = 0; U64 t1_req = 0, t1_opt = 0; };
    static bool vkTexStatsStep(F64 budget_s, VKTexStats& out);
    static U32 sVKScaledDown, sVKScaleInFlight;
    static U64 sVKDropped, sVKResourced, sVKResourceFailed;   // #707: copies dropped / re-sourced / abandoned (lifetime)
    // TEXTURE MEMORY TIERS (#405 / #707). Tier-1 (decoded RAM copy) bytes split into
    // LEASED (a consumer holds the copy: vkCpuLeaseMask != 0, by reason, see
    // LLTextureData::EVKLease; plus everything outside the fetched list -- fonts, media,
    // dynamic) and UNLEASED (waiting out the drop grace / the tier-2 capture, or kept as
    // the Auto big-RAM fast path). Per-reason bytes in `by_reason` (may be null).
    // Cached ~2 s (walks the texture list). Main thread.
    static void getVKTier1Bytes(U64& required, U64& optional, U64* by_reason = nullptr);
    // Tier-1 retention policy right now: true = keep unleased copies of world textures at
    // the demand size (Auto mode, system memory comfortable, retained copies under 3/4 of
    // the governor's RAM-scaled allowance: the fastest re-upload); false = drop them once
    // the GPU holds the texture (#707).
    static bool vkKeepTier1();
    // SlipstreamTextureMemoryMode == 1 ("Prefer disk cache": lowest RAM), or Auto on a
    // machine with less RAM than SlipstreamTextureDiskBelowRAMGB.
    static bool vkPreferDisk();

    void clearFetchedResults(); //clear all fetched results, for debug use.

    // Override the computation of discard levels if we know the exact output
    // size of the image.  Used for UI textures to not decode, even if we have
    // more data.
    /*virtual*/ void setKnownDrawSize(S32 width, S32 height) override;

    // Set the debug text of all Viewer Objects associated with this texture
    // to the specified text
    void setDebugText(const std::string& text);

    void setIsMissingAsset(bool is_missing = true);
    /*virtual*/ bool isMissingAsset() const override { return mIsMissingAsset; }

    // returns dimensions of original image for local files (before power of two scaling)
    // and returns 0 for all asset system images
    S32 getOriginalWidth() { return mOrigWidth; }
    S32 getOriginalHeight() { return mOrigHeight; }

    bool isInImageList() const {return mInImageList ;}
    void setInImageList(bool flag) {mInImageList = flag ;}

    LLFrameTimer* getLastPacketTimer() {return &mLastPacketTimer;}

    U32 getFetchPriority() const { return mFetchPriority ;}
    F32 getDownloadProgress() const {return mDownloadProgress ;}

    void destroyRawImage();
    bool needsToSaveRawImage();

    const std::string& getUrl() const {return mUrl;}
    //---------------
    bool isDeleted() ;
    bool getUseDiscard() const { return mUseMipMaps && !mDontDiscard; }
    //---------------

    void setForSculpt();
    bool forSculpt() const {return mForSculpt;}
    bool isForSculptOnly() const;

    //raw image management
    LLImageRaw* getRawImage()const { return mRawImage ;}
    S32         getRawImageLevel() const {return mRawDiscardLevel;}
    bool        isRawImageValid()const { return mIsRawImageValid ; }
    void        forceToSaveRawImage(S32 desired_discard = 0, F32 kept_time = 0.f) ;

    // #655: the VK stand-in for LL's GL readbackRawImage(): the best discard level a raw
    // copy can be made at from CPU-side pixels (saved raw image, else the texture's
    // retained VK CPU copy), MAX_DISCARD_LEVEL + 1 if none; and make that copy.
    S32         vkReadbackDiscard() const;
    bool        vkReadbackRawImage();

    // CPU COPY LEASES (#707, see LLTextureData::EVKLease). The reasons this texture's
    // decoded copy must stay: the image's own (explicit leases, media, alpha job) plus
    // what only the texture sees (not re-sourceable, the world renderer's image not
    // covering the demand, the UI not having uploaded it, raw callbacks waiting, own
    // avatar local compositing). 0 = nobody needs it: vkCpuCopyStep may drop it.
    U32         vkCpuLeaseMask() const;
    // Fetched by UUID / URL with nothing injected: a decode brings the same pixels back.
    bool        vkCpuResourceable() const;
    // Policy step (every processTextureStats visit): drop an unleased copy once its pixels
    // are safe elsewhere, keep a pending re-source asking for its decode. True = dropped.
    bool        vkCpuCopyStep();
    // An explicit async re-source of a dropped copy (decode at `discard`, -1 = the level the
    // copy had): the fetcher re-decodes from the local cache even when LL's bookkeeping
    // says that level is loaded. Never a thumbnail.
    void        vkRequestCpuCopy(S32 discard);
    void        vkRequestCpuCopy() override { vkRequestCpuCopy(-1); }
    bool        vkResourcePending() const { return mVKResourceDiscard >= 0; }

    void        destroySavedRawImage() ;
    LLImageRaw* getSavedRawImage() ;
    S32         getSavedRawImageLevel() const {return mSavedRawDiscardLevel; }

    const LLImageRaw* getSavedRawImage() const;
    const LLImageRaw* getAuxRawImage() const { return mAuxRawImage; }
    bool        hasSavedRawImage() const ;
    F32         getElapsedLastReferencedSavedRawImageTime() const ;
    bool        isFullyLoaded() const;

    bool        hasFetcher() const { return mHasFetcher;}
    bool        isFetching() const { return mIsFetching;}
    void        setCanUseHTTP(bool can_use_http) {mCanUseHTTP = can_use_http;}

    void        forceToDeleteRequest();

    /*virtual*/bool  isActiveFetching() override; //is actively in fetching by the fetching pipeline.


    bool mCreatePending = false;    // if true, this is in gTextureList.mCreateTextureList

    LLUUID      getUploader();
    LLDate      getUploadTime();
    std::string getComment();

protected:
    S32 getCurrentDiscardLevelForFetching() ;
    void forceToRefetchTexture(S32 desired_discard = 0, F32 kept_time = 60.f);

private:
    void init(bool firstinit) ;
    void cleanup() ;

    bool processFetchResults(S32& desired_discard, S32 current_discard, S32 fetch_discard, F32 decode_priority);

public:
    // Vulkan residency: true when applyVKDemand will own this texture's desired discard
    // and fetch priority (so LL's per-face pixel-area scan would be overwritten).
    bool vkDemandActive() const;
    // Vulkan: demand-driven, nothing in flight (fetch/decode/create/callbacks/downscale),
    // resident discard meets the desired one, and the demand hasn't changed since LL's
    // list last processed it -- the per-frame list update can skip it (a slow rotation
    // still visits it for flush/delete maintenance).
    bool vkSettled();
    // Why this texture is NOT settled (0 = settled); for the fetch-loop histogram.
    enum EVKUnsettled : U8 { VKU_SETTLED = 0, VKU_NOT_DRIVEN, VKU_FETCHING, VKU_CREATE, VKU_FASTCACHE,
                             VKU_CALLBACKS, VKU_SHARPEN, VKU_DEMAND_CHANGED, VKU_COUNT };
    U8   vkUnsettledReason();
    static U32 sVKUnsettledHist[VKU_COUNT];
    static U32 sVKNotDrivenBoosted[2];   // NOT_DRIVEN split: [0] BOOST_NONE, [1] boosted
    bool mVKIdleOwned = false;          // never-demanded, discard owned by the thumbnail rule
    void vkMarkSeen() { mVKDemandSeen = mVKDemand; }
    U32  getVKDemand() const { return mVKDemand; }   // diagnostics
    F32  getVKDemandAge() const { return mVKDemandTime > 0.f ? sCurrentTime - mVKDemandTime : -1.f; }
    U32  getVKGpuTexels() const { return mVKGpuTexels; }
    std::string fetchDiagString();                    // diagnostics (RenderVKBakeDiag)
    U8   mVKSkipCount = 0;
    bool mVKDirtyQueued = false;
    // Textures whose VK demand changed since last processed: the list update handles
    // them first, instead of waiting for the ~20-frame rotation to come round.
    static std::vector<LLPointer<LLViewerFetchedTexture>> sVKDirty;
    // #485: active trim while the retained CPU copies are over their allowance: run the
    // #707 drop rule (vkCpuCopyStep) now instead of on the maintenance visit. True if the
    // copy was dropped.
    bool vkTrimIdleCpuCopy();
    // #485 NETWORK DRAW DISTANCE FETCH GATE: true when every world object using this
    // texture (face / sculpt / light users) is farther from the agent HORIZONTALLY (X/Y
    // only -- a sim's full height stays eligible) than the network draw distance (the
    // AgentUpdate Far, speed-adaptive) plus its radius and SLFetchNetDDGateMargin. A
    // gated texture makes no new request and withdraws a queued one (cancel before send);
    // what is loaded stays. Own avatar / attachments / HUDs, avatar bakes, boosted UI /
    // preview / map textures and textures with no known user are never gated.
    // Re-evaluated at most once a second per texture.
    bool vkNetGated();
    static U64 sNetGateSkipped;     // lifetime: gate evaluations that held a texture back
    static U64 sNetGateDropped;     // lifetime: active fetches withdrawn by the gate
    static U32 sNetGateGated;       // textures gated at their last evaluation (live)
protected:
    F32  mNetGateCheckTime = -1.e9f;
    bool mNetGated = false;
protected:
    // Vulkan residency (see setVKDemand): true when the demand set mDesiredDiscardLevel.
    bool applyVKDemand();
    // Post an off-thread box downscale of the retained VK CPU copy to target_discard.
    // 1 started, 0 busy (retry later), -1 impossible for this target (tiny / writable /
    // no CPU copy): the caller stops retrying until the target discard changes.
    S32  vkScaleDownCpuCopy(S32 target_discard);
    S8   mVKNoScaleDiscard = -1, mVKNoScaleCur = -1;   // (target, current) found unscalable
    U32  mVKDemand = 0;
    struct VKDecodeDemand
    {
        U32 texels = 0;
        U32 pass = 0;
        F32 time = 0.f;
    };
    VKDecodeDemand mVKDecodeDemand[2];
    U32  mVKGpuTexels = 0;       // renderer's resident GPU size (this demand pass)
    bool mVKT2Safe = false;      // #707: tier-2 captured or ineligible (this demand pass)
    bool mVKFromRaw = false;     // #707: created from raw pixels (never re-sourceable)
    S8   mVKResourceDiscard = -1;  // #707: re-source requested at this discard (-1 = none)
    F32  mVKResourceSince = -1.f;  // #707: since when (a re-source that never lands is dropped)
    F32  mVKDropReadySince = -1.f; // #707: unleased since (drop grace)
    F32  mVKCaptureWaitSince = -1.f; // #707: waiting for the tier-2 capture since
    bool vkWorldLive() const;      // #707: the world renderer asked for it recently
    bool vkGpuCoversDemand() const;  // #707: its GPU image (or tier-2 chain) covers the demand
    U32  mVKDemandSeen = ~0u;
    U32  mVKDemandPass = 0;
    F32  mVKDemandTime = -1.e9f;
    F32  mVKOverSince = -1.f;      // since when the CPU copy has been sharper than desired
    bool mVKScalePending = false;

private:

    void saveRawImage() ;

private:
    bool  mFullyLoaded;
    bool  mForceCallbackFetch;

protected:
    S32 mOrigWidth;
    S32 mOrigHeight;

    // Override the computation of discard levels if we know the exact output size of the image.
    // Used for UI textures to not decode, even if we have more data.
    S32 mKnownDrawWidth;
    S32 mKnownDrawHeight;
    bool mKnownDrawSizeChanged ;
    std::string mUrl;

    S32 mLastWorkerDiscardLevel;
    S32 mRequestedDiscardLevel;
    F32 mRequestedDownloadPriority;
    S32 mFetchState;
    S32 mLastFetchState = -1; // DEBUG
    U32 mFetchPriority;
    F32 mDownloadProgress;
    F32 mFetchDeltaTime;
    F32 mRequestDeltaTime;
    S32 mMinDiscardLevel;
    S8  mDesiredDiscardLevel;           // The discard level we'd LIKE to have - if we have it and there's space
    S8  mMinDesiredDiscardLevel;    // The minimum discard level we'd like to have

    bool mNeedsAux;                 // We need to decode the auxiliary channels
    bool mHasAux;                    // We have aux channels
    bool mDecodingAux;              // Are we decoding high components
    bool mIsRawImageValid;
    bool mHasFetcher;               // We've made a fecth request
    bool mIsFetching;               // Fetch request is active
    bool mCanUseHTTP;              //This texture can be fetched through http if true.
    LLCore::HttpStatus mLastHttpGetStatus; // Result of the most recently completed http request for this texture.

    FTType mFTType; // What category of image is this - map tile, server bake, etc?
    mutable bool mIsMissingAsset;       // True if we know that there is no image asset with this image id in the database.

    typedef std::list<LLLoadedCallbackEntry*> callback_list_t;
    S8              mLoadedCallbackDesiredDiscardLevel;
    bool            mPauseLoadedCallBacks;
    callback_list_t mLoadedCallbackList;
    F32             mLastCallBackActiveTime;

    LLPointer<LLImageRaw> mRawImage;
    S32 mRawDiscardLevel = -1;

    // Used ONLY for cloth meshes right now.  Make SURE you know what you're
    // doing if you use it for anything else! - djs
    LLPointer<LLImageRaw> mAuxRawImage;

    //keep a copy of mRawImage for some special purposes
    //when mForceToSaveRawImage is set.
    bool mForceToSaveRawImage ;
    bool mSaveRawImage;
    LLPointer<LLImageRaw> mSavedRawImage;
    S32 mSavedRawDiscardLevel;
    S32 mDesiredSavedRawDiscardLevel;
    F32 mLastReferencedSavedRawImageTime ;
    F32 mKeptSavedRawImageTime ;

    LLHost mTargetHost; // if invalid, just request from agent's simulator

    // Timers
    LLFrameTimer mLastPacketTimer;      // Time since last packet.
    LLFrameTimer mStopFetchingTimer;    // Time since mDecodePriority == 0.f.

    // #309: what doLoadedCallbacks() looked at, as of when it last ran
    struct LoadedCallbackState
    {
        S32 mDiscard = 0, mMaxDiscard = 0, mRawDiscard = 0;
        U32 mCallbacks = 0, mAdds = 0;
        F32 mActiveTime = 0.f;
        bool mNeedsCreate = false, mPaused = false, mMissing = false, mFetching = false;
        bool mRawValid = false, mHaveRaw = false, mNeedsAux = false, mHaveAux = false, mForceFetch = false;
        bool operator==(const LoadedCallbackState& o) const
        {
            return mDiscard == o.mDiscard && mMaxDiscard == o.mMaxDiscard && mRawDiscard == o.mRawDiscard
                && mCallbacks == o.mCallbacks && mAdds == o.mAdds && mActiveTime == o.mActiveTime
                && mNeedsCreate == o.mNeedsCreate && mPaused == o.mPaused && mMissing == o.mMissing
                && mFetching == o.mFetching && mRawValid == o.mRawValid && mHaveRaw == o.mHaveRaw
                && mNeedsAux == o.mNeedsAux && mHaveAux == o.mHaveAux && mForceFetch == o.mForceFetch;
        }
    };
    LoadedCallbackState loadedCallbackState() const;
    LoadedCallbackState mLoadedCallbackLast;
    F32 mLoadedCallbackLastTime = -1.f;     // sCurrentTime it last ran (-1 = never)
    U32 mLoadedCallbackAdds = 0;            // setLoadedCallback() count (a swap keeps the size)
    F32 mFetchPriorityPosted = -1.f;    // #305: last priority given to the fetcher (-1 = none)

    bool  mInImageList;             // true if image is in list (in which case don't reset priority!)
    // This needs to be atomic, since it is written both in the main thread
    // and in the GL image worker thread... HB
    LLAtomicBool  mNeedsCreateTexture;

    bool   mForSculpt ; //a flag if the texture is used as sculpt data.
    bool   mIsFetched ; //is loaded from remote or from cache, not generated locally.

    std::map<S8, std::string> mComment;

public:
    static F32 sMaxVirtualSize; //maximum possible value of mMaxVirtualSize
    static LLPointer<LLViewerFetchedTexture> sMissingAssetImagep;   // Texture to show for an image asset that is not in the database
    static LLPointer<LLViewerFetchedTexture> sWhiteImagep;  // Texture to show NOTHING (whiteness)
    static LLPointer<LLViewerFetchedTexture> sDefaultImagep; // "Default" texture for error cases, the only case of fetched texture which is generated in local.
    static LLPointer<LLViewerFetchedTexture> sFlatNormalImagep; // Flat normal map denoting no bumpiness on a surface
    static LLPointer<LLViewerFetchedTexture> sDefaultIrradiancePBRp; // PBR: irradiance
    static LLPointer<LLViewerFetchedTexture> sDefaultParticleImagep; // Default particle texture

    // not sure why, but something is iffy about the loading of this particular texture, use the accessor instead of accessing directly
    static LLPointer<LLViewerFetchedTexture> sSmokeImagep; // Old "Default" translucent texture
    static LLViewerFetchedTexture* getSmokeImage();
// [SL:KB] - Patch: Render-TextureToggle (Catznip-4.0)
    static LLPointer<LLViewerFetchedTexture> sDefaultDiffuseImagep;
// [/SL:KB]
};

//
//the image data is fetched from remote or from local cache
//the resolution of the texture is adjustable: depends on the view-dependent parameters.
//
class LLViewerLODTexture : public LLViewerFetchedTexture
{
protected:
    /*virtual*/ ~LLViewerLODTexture(){}

public:
    LLViewerLODTexture(const LLUUID& id, FTType f_type, const LLHost& host = LLHost(), bool usemipmaps = true);
    LLViewerLODTexture(const std::string& url, FTType f_type, const LLUUID& id, bool usemipmaps = true);

    S8 getType() const override;
    // Process image stats to determine priority/quality requirements.
    void processTextureStats() override;
    bool isUpdateFrozen() ;


private:
    void init(bool firstinit) ;
};

//
//the image data is fetched from the media pipeline periodically
//the resolution of the texture is also adjusted by the media pipeline
//
class LLViewerMediaTexture : public LLViewerTexture
{
protected:
    /*virtual*/ ~LLViewerMediaTexture() ;

public:
    LLViewerMediaTexture(const LLUUID& id, bool usemipmaps = true, LLTextureData* gl_image = NULL) ;

    /*virtual*/ S8 getType() const;
    void reinit(bool usemipmaps = true);

    bool  getUseMipMaps() {return mUseMipMaps ; }
    void  setUseMipMaps(bool mipmap) ;

    void setPlaying(bool playing) ;
    bool isPlaying() const {return mIsPlaying;}
    void setMediaImpl() ;

    virtual bool isViewerMediaTexture() const { return true; }
    // Vulkan UI accounting (#405): media drawn by the UI (web floaters, media controls).
    /*virtual*/ U8 getVKUiKind() const;

    void initVirtualSize() ;
    void invalidateMediaImpl() ;

    void addMediaToFace(LLFace* facep) ;
    void removeMediaFromFace(LLFace* facep) ;

    /*virtual*/ void addFace(U32 ch, LLFace* facep) ;
    /*virtual*/ void removeFace(U32 ch, LLFace* facep) ;

    /*virtual*/ F32  getMaxVirtualSize();

private:
    void switchTexture(U32 ch, LLFace* facep) ;
    bool findFaces() ;
    void stopPlaying() ;

private:
    //
    //an instant list, recording all faces referencing or can reference to this media texture.
    //NOTE: it is NOT thread safe.
    //
    std::list< LLFace* > mMediaFaceList ;

    //an instant list keeping all textures which are replaced by the current media texture,
    //is only used to avoid the removal of those textures from memory.
    std::list< LLPointer<LLViewerTexture> > mTextureList ;

    LLViewerMediaImpl* mMediaImplp ;
    bool mIsPlaying ;
    U32  mUpdateVirtualSizeTime ;

public:
    static void updateClass() ;
    static void cleanUpClass() ;

    static LLViewerMediaTexture* findMediaTexture(const LLUUID& media_id) ;
    static void removeMediaImplFromTexture(const LLUUID& media_id) ;

private:
    typedef std::map< LLUUID, LLPointer<LLViewerMediaTexture> > media_map_t ;
    static media_map_t sMediaMap ;
};

//just an interface class, do not create instance from this class.
class LLViewerTextureManager
{
private:
    //make the constructor private to preclude creating instances from this class.
    LLViewerTextureManager(){}

public:
    //texture pipeline tester
    static LLTexturePipelineTester* sTesterp ;

    //returns NULL if tex is not a LLViewerFetchedTexture nor derived from LLViewerFetchedTexture.
    static LLViewerFetchedTexture*    staticCastToFetchedTexture(LLTexture* tex, bool report_error = false) ;

    //
    //"find-texture" just check if the texture exists, if yes, return it, otherwise return null.
    //
    static void                       findFetchedTextures(const LLUUID& id, std::vector<LLViewerFetchedTexture*> &output);
    static void                       findTextures(const LLUUID& id, std::vector<LLViewerTexture*> &output);
    static LLViewerFetchedTexture*    findFetchedTexture(const LLUUID& id, S32 tex_type);
    static LLViewerMediaTexture*      findMediaTexture(const LLUUID& id) ;

    static LLViewerMediaTexture*      createMediaTexture(const LLUUID& id, bool usemipmaps = true, LLTextureData* gl_image = NULL) ;

    //
    //"get-texture" will create a new texture if the texture does not exist.
    //
    static LLViewerMediaTexture*      getMediaTexture(const LLUUID& id, bool usemipmaps = true, LLTextureData* gl_image = NULL) ;

    static LLPointer<LLViewerTexture> getLocalTexture(bool usemipmaps = true, bool generate_gl_tex = true);
    static LLPointer<LLViewerTexture> getLocalTexture(const LLUUID& id, bool usemipmaps, bool generate_gl_tex = true) ;
    static LLPointer<LLViewerTexture> getLocalTexture(const LLImageRaw* raw, bool usemipmaps) ;
    static LLPointer<LLViewerTexture> getLocalTexture(const U32 width, const U32 height, const U8 components, bool usemipmaps, bool generate_gl_tex = true) ;

    static LLViewerFetchedTexture* getFetchedTexture(const LLImageRaw* raw, FTType type, bool usemipmaps);

    static LLViewerFetchedTexture* getFetchedTexture(const LLUUID &image_id,
                                     FTType f_type = FTT_DEFAULT,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,     // Get the requested level immediately upon creation.
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLPixelFormat internal_format = LLPixelFormat::NONE,
                                     LLPixelFormat primary_format = LLPixelFormat::NONE,
                                     LLHost request_from_host = LLHost()
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromFile(const std::string& filename,
                                     FTType f_type = FTT_LOCAL_FILE,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLPixelFormat internal_format = LLPixelFormat::NONE,
                                     LLPixelFormat primary_format = LLPixelFormat::NONE,
                                     const LLUUID& force_id = LLUUID::null
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromUrl(const std::string& url,
                                     FTType f_type,
                                     bool usemipmap = true,
                                     LLViewerTexture::EBoostLevel boost_priority = LLGLTexture::BOOST_NONE,
                                     S8 texture_type = LLViewerTexture::FETCHED_TEXTURE,
                                     LLPixelFormat internal_format = LLPixelFormat::NONE,
                                     LLPixelFormat primary_format = LLPixelFormat::NONE,
                                     const LLUUID& force_id = LLUUID::null
                                     );

    static LLViewerFetchedTexture* getFetchedTextureFromHost(const LLUUID& image_id, FTType f_type, LLHost host) ;

    // decode a given image data according to given mime type
    // WARNING: caller is responsible for deleting the returned raw image
    static LLImageRaw* getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype);

    // decode given image data according to given mime type
    // WARNING: caller is responsible for deleting the returned image
    static LLViewerFetchedTexture* getFetchedTextureFromMemory(const U8* data, U32 size, std::string_view mimetype);

    static void init() ;
    static void cleanup() ;
};
//
//this class is used for test/debug only
//it tracks the activities of the texture pipeline
//records them, and outputs them to log files
//
class LLTexturePipelineTester : public LLMetricPerformanceTesterWithSession
{
    enum
    {
        MIN_LARGE_IMAGE_AREA = 262144  //512 * 512
    };
public:
    LLTexturePipelineTester() ;
    ~LLTexturePipelineTester() ;

    void update();
    void updateTextureBindingStats(const LLViewerTexture* imagep) ;
    void updateTextureLoadingStats(const LLViewerFetchedTexture* imagep, const LLImageRaw* raw_imagep, bool from_cache) ;
    void updateGrayTextureBinding() ;
    void setStablizingTime() ;

private:
    void reset() ;
    void updateStablizingTime() ;

    /*virtual*/ void outputTestRecord(LLSD* sd) ;

private:
    bool mPause ;
private:
    bool mUsingDefaultTexture;            //if set, some textures are still gray.

    U32Bytes mTotalBytesUsed ;                     //total bytes of textures bound/used for the current frame.
    U32Bytes mTotalBytesUsedForLargeImage ;        //total bytes of textures bound/used for the current frame for images larger than 256 * 256.
    U32Bytes mLastTotalBytesUsed ;                 //total bytes of textures bound/used for the previous frame.
    U32Bytes mLastTotalBytesUsedForLargeImage ;    //total bytes of textures bound/used for the previous frame for images larger than 256 * 256.

    //
    //data size
    //
    U32Bytes mTotalBytesLoaded ;               //total bytes fetched by texture pipeline
    U32Bytes mTotalBytesLoadedFromCache ;      //total bytes fetched by texture pipeline from local cache
    U32Bytes mTotalBytesLoadedForLargeImage ;  //total bytes fetched by texture pipeline for images larger than 256 * 256.
    U32Bytes mTotalBytesLoadedForSculpties ;   //total bytes fetched by texture pipeline for sculpties

    //
    //time
    //NOTE: the error tolerances of the following timers is one frame time.
    //
    F32 mStartFetchingTime ;
    F32 mTotalGrayTime ;                  //total loading time when no gray textures.
    F32 mTotalStablizingTime ;            //total stablizing time when texture memory overflows
    F32 mStartTimeLoadingSculpties ;      //the start moment of loading sculpty images.
    F32 mEndTimeLoadingSculpties ;        //the end moment of loading sculpty images.
    F32 mStartStablizingTime ;
    F32 mEndStablizingTime ;

private:
    //
    //The following members are used for performance analyzing
    //
    class LLTextureTestSession : public LLTestSession
    {
    public:
        LLTextureTestSession() ;
        /*virtual*/ ~LLTextureTestSession() ;

        void reset() ;

        F32 mTotalGrayTime ;
        F32 mTotalStablizingTime ;
        F32 mStartTimeLoadingSculpties ;
        F32 mTotalTimeLoadingSculpties ;

        S32 mTotalBytesLoaded ;
        S32 mTotalBytesLoadedFromCache ;
        S32 mTotalBytesLoadedForLargeImage ;
        S32 mTotalBytesLoadedForSculpties ;

        typedef struct _texture_instant_preformance_t
        {
            S32 mAverageBytesUsedPerSecond ;
            S32 mAverageBytesUsedForLargeImagePerSecond ;
            F32 mAveragePercentageBytesUsedPerSecond ;
            F32 mTime ;
        }texture_instant_preformance_t ;
        std::vector<texture_instant_preformance_t> mInstantPerformanceList ;
        S32 mInstantPerformanceListCounter ;
    };

    /*virtual*/ LLMetricPerformanceTesterWithSession::LLTestSession* loadTestSession(LLSD* log) ;
    /*virtual*/ void compareTestSessions(llofstream* os) ;
};

#endif
