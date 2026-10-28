/**
 * @file llcelshadevk.cpp
 * @brief Avatar mask (#549) + built-in cel shading (#550): see llcelshadevk.h and the
 *        shaders clean_cel_common.glsl, clean_avatar_id.frag, clean_cel_filter.comp,
 *        clean_avatar_id_view.frag.
 *
 * Also defines the LLCleanRendererVK members declared for this module
 * (writeCelDescriptors, recordCelFrameStart, recordCelPasses, destroyCel and the avatar
 * mask contract accessors), so the shared renderer file only carries the call sites.
 */
#include "linden_common.h"

#include "llcelshadevk.h"
#include "llcleanrenderervk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llagent.h"
#include "llcallingcard.h"      // LLAvatarTracker (friends scope)
#include "llcharacter.h"
#include "llcleanskinstore.h"
#include "llerror.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
std::set<LLUUID>& chosenAvatars()
{
    static std::set<LLUUID> s;
    return s;
}
uint32_t s_chosenGen = 1;

VkShaderModule celModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = srcStage; m.srcAccessMask = srcAccess;
    m.dstStageMask = dstStage; m.dstAccessMask = dstAccess;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Commands per compacted worn-attachment list (clean_cel_filter.comp): an avatar wearing
// 60 rigid attachments of ~8 faces is ~500 draws; 32 such avatars on screen fit.
constexpr uint32_t kFilterListCap = 16384;
constexpr VkDeviceSize kFilterHdr = 16;
constexpr VkDeviceSize kDrawCmdBytes = 20;   // VkDrawIndexedIndirectCommand

struct FilterPush { uint32_t src_cap, list_cap, inst_count, pad; };
struct ResolvePush { int32_t dsize[2]; int32_t rsize[2]; float ratio[2]; float jitter[2]; };
// clean_cel_outline.frag Push (80 B).
struct OutlinePush { float dims[4]; float lin[4]; float color[4]; float p0[4]; float p1[4]; };
static_assert(sizeof(OutlinePush) == 80, "OutlinePush");
} // namespace

// ---------------------------------------------------------------------------
// LLCelShadeVK
// ---------------------------------------------------------------------------

void LLCelShadeVK::toggleChosen(const LLUUID& id)
{
    std::set<LLUUID>& s = chosenAvatars();
    if (!s.erase(id)) s.insert(id);
    ++s_chosenGen;
}

bool LLCelShadeVK::isChosen(const LLUUID& id)
{
    return chosenAvatars().count(id) != 0;
}

// Avatar context menu ("Cel shade this avatar", llviewermenu.cpp; declared there so the
// menu TU needs no Vulkan headers). Adding an avatar turns cel shading on and, unless
// the scope already covers every avatar, switches it to "chosen avatars".
void sl_cel_toggle_avatar(const LLUUID& id)
{
    LLCelShadeVK::toggleChosen(id);
    if (!LLCelShadeVK::isChosen(id)) return;
    gSavedSettings.setBOOL("RenderVKCel", true);
    const U32 scope = gSavedSettings.getU32("RenderVKCelScope");
    if (scope != LLCelShadeVK::SCOPE_SCENE && scope != LLCelShadeVK::SCOPE_AVATARS)
        gSavedSettings.setU32("RenderVKCelScope", LLCelShadeVK::SCOPE_CHOSEN);
}

bool sl_cel_avatar_chosen(const LLUUID& id)
{
    return LLCelShadeVK::isChosen(id);
}

void LLCelShadeVK::destroy(VkDevice dev)
{
    if (!mCtx) return;
    vkDeviceWaitIdle(dev);
    for (VkPipeline& p : mIdPipe) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
    for (VkPipeline* p : {&mFilterPipe, &mResolvePipe, &mDebugPipe, &mOutlinePipe})
    {
        if (*p) vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (mFilterLayout) vkDestroyPipelineLayout(dev, mFilterLayout, nullptr);
    if (mViewLayout) vkDestroyPipelineLayout(dev, mViewLayout, nullptr);
    mFilterLayout = mViewLayout = VK_NULL_HANDLE;
    mPool.destroy();
    mFilterSet[0] = mFilterSet[1] = mViewSet[0] = mViewSet[1] = VK_NULL_HANDLE;
    if (mFilterDsl) vkdDestroySetLayout(dev, mFilterDsl);
    if (mViewDsl) vkdDestroySetLayout(dev, mViewDsl);
    mFilterDsl = mViewDsl = VK_NULL_HANDLE;
    if (mSampler) vkDestroySampler(dev, mSampler, nullptr);
    mSampler = VK_NULL_HANDLE;
    if (mId.image) mCtx->destroyImage(mId);
    if (mIdDisp.image) mCtx->destroyImage(mIdDisp);
    mId = GpuImage{}; mIdDisp = GpuImage{};
    mIdW = mIdH = mIdDispW = mIdDispH = 0;
    if (mFilterBuf.buffer) mCtx->destroyBuffer(mFilterBuf);
    if (mParamsBuf.buffer) mCtx->destroyBuffer(mParamsBuf);
    mFilterBuf = GpuBuffer{}; mParamsBuf = GpuBuffer{};
    mFilterCap = 0;
    mIdPipesFailed = false;
    mHdrFormatBuilt = VK_FORMAT_UNDEFINED;
}

// ---------------------------------------------------------------------------
// LLCleanRendererVK members (declared in llcleanrenderervk.h, "AVATAR MASK + CEL")
// ---------------------------------------------------------------------------

VkImageView LLCleanRendererVK::getAvatarMaskView() const
{
    if (!mCel || !mCel->mMaskValid) return VK_NULL_HANDLE;
    return mCel->mMaskIsRender ? mCel->mId.view : mCel->mIdDisp.view;
}

bool LLCleanRendererVK::hasAvatarMask() const
{
    return mCel && mCel->mMaskValid && getAvatarMaskView() != VK_NULL_HANDLE;
}

uint64_t LLCleanRendererVK::avatarMaskGen() const
{
    return mCel ? mCel->mGen : 0;
}

void LLCleanRendererVK::requestAvatarMask(bool want)
{
    mCelFxWantsMask = want;
    if (mCel) mCel->mFxWantsMask = want;
}

void LLCleanRendererVK::destroyCel()
{
    if (!mCel) return;
    if (mCtx) mCel->destroy(mCtx->device());
    delete mCel;
    mCel = nullptr;
}

void LLCleanRendererVK::writeCelDescriptors()
{
    if (!mCtx || !mDsetDraw0) return;
    if (!mCel)
    {
        mCel = new LLCelShadeVK();
        mCel->mCtx = mCtx;
        mCel->mFxWantsMask = mCelFxWantsMask;
    }
    LLCelShadeVK& c = *mCel;
    if (!c.mParamsBuf.buffer)
    {
        VkMemScope mem(VKMEM_SCENE);   // #407
        c.mParamsBuf = mCtx->createBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        c.mParamsDirty = true;   // the first recordCelFrameStart fills it
        if (!c.mParamsBuf.buffer) return;
    }
    // 38 aliases the parameter buffer until the skin meta exists (CEL_META clear: unread).
    const VkDescriptorBufferInfo pb{c.mParamsBuf.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo mb{mSkinMetaSSBO.buffer ? mSkinMetaSSBO.buffer : c.mParamsBuf.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mDsetDraw0;
        w[i].dstBinding = 37u + (uint32_t)i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = i == 0 ? &pb : &mb;
    }
    vkUpdateDescriptorSets(mCtx->device(), 2, w, 0, nullptr);
    c.mParamsDirty = true;   // CEL_META follows the binding at the next frame start
}

void LLCleanRendererVK::recordCelFrameStart(VkCommandBuffer cmd)
{
    if (!mCel || !mCel->mParamsBuf.buffer) return;
    LLCelShadeVK& c = *mCel;
    c.mMaskValid = false;

    static LLCachedControl<bool> s_cel(gSavedSettings, "RenderVKCel", false);
    static LLCachedControl<U32>  s_scope(gSavedSettings, "RenderVKCelScope", 1);
    static LLCachedControl<bool> s_dbg(gSavedSettings, "RenderVKCelDebugMask", false);
    static LLCachedControl<U32>  s_view(gSavedSettings, "RenderVKDebugView", 0);

    c.mCelOn = s_cel;
    c.mScope = std::min<U32>(s_scope, LLCelShadeVK::SCOPE_FRIENDS);
    c.mDebugNow = s_dbg || s_view == 22u;
    // Toon lighting + outline controls (#550).
    static LLCachedControl<U32>     s_bands(gSavedSettings, "RenderVKCelBands", 2);
    static LLCachedControl<F32>     s_soft(gSavedSettings, "RenderVKCelSoftness", 0.1f);
    static LLCachedControl<F32>     s_flat(gSavedSettings, "RenderVKCelAmbientFlatten", 0.6f);
    static LLCachedControl<LLColor4> s_shadow(gSavedSettings, "RenderVKCelShadowTint", LLColor4(0.35f, 0.3f, 0.5f, 1.f));
    static LLCachedControl<F32>     s_shadowAmt(gSavedSettings, "RenderVKCelShadowFill", 0.3f);
    static LLCachedControl<F32>     s_rim(gSavedSettings, "RenderVKCelRim", 0.35f);
    static LLCachedControl<F32>     s_rimW(gSavedSettings, "RenderVKCelRimWidth", 0.3f);
    static LLCachedControl<F32>     s_spec(gSavedSettings, "RenderVKCelSpecular", 0.25f);
    static LLCachedControl<F32>     s_specT(gSavedSettings, "RenderVKCelSpecularThreshold", 0.5f);
    static LLCachedControl<F32>     s_ow(gSavedSettings, "RenderVKCelOutlineWidth", 1.5f);
    static LLCachedControl<LLColor4> s_oc(gSavedSettings, "RenderVKCelOutlineColor", LLColor4(0.f, 0.f, 0.f, 1.f));
    static LLCachedControl<F32>     s_oi(gSavedSettings, "RenderVKCelOutlineIntensity", 1.f);
    static LLCachedControl<F32>     s_ofade(gSavedSettings, "RenderVKCelOutlineFadeDistance", 96.f);
    static LLCachedControl<F32>     s_odepth(gSavedSettings, "RenderVKCelOutlineDepthThreshold", 0.04f);
    static LLCachedControl<F32>     s_ocrease(gSavedSettings, "RenderVKCelOutlineCreases", 0.02f);
    c.mOutlineNow = c.mCelOn && (F32)s_ow > 0.f && ((LLColor4)s_oc).mV[3] > 0.f;
    c.mOutlineWidth = llclamp((F32)s_ow, 0.f, 16.f);
    const LLColor4 oc = s_oc;
    for (int i = 0; i < 3; ++i) c.mOutlineColor[i] = oc.mV[i] * oc.mV[i] * std::max((F32)s_oi, 0.f);   // ~sRGB -> linear
    c.mOutlineColor[3] = llclamp(oc.mV[3], 0.f, 1.f);
    c.mOutlineFade = std::max((F32)s_ofade, 0.f);
    c.mOutlineDepth = std::max((F32)s_odepth, 0.001f);
    c.mOutlineCrease = std::max((F32)s_ocrease, 0.f);
    c.mIdsNow = c.mOutlineNow || c.mDebugNow || c.mFxWantsMask;

    LLCelShadeVK::Params p = c.mParams;
    p.flags = 0;
    p.bands = llclamp((U32)s_bands, 1u, 8u);
    p.softness = llclamp((F32)s_soft, 0.f, 1.f);
    p.flatten = llclamp((F32)s_flat, 0.f, 1.f);
    {
        const LLColor4 st = s_shadow;
        for (int i = 0; i < 3; ++i) p.shadow[i] = st.mV[i];
        p.shadow[3] = llclamp((F32)s_shadowAmt, 0.f, 1.f);
        for (int i = 0; i < 3; ++i) p.rim[i] = std::max((F32)s_rim, 0.f);
        p.rim[3] = llclamp((F32)s_rimW, 0.f, 1.f);
        p.spec[0] = std::max((F32)s_specT, 0.f);
        p.spec[1] = 0.1f;
        p.spec[2] = std::max((F32)s_spec, 0.f);
        p.spec[3] = 0.f;
    }
    if (c.mCelOn) p.flags |= LLCelShadeVK::kFlagLight;
    if (c.mCelOn && p.spec[2] > 0.f) p.flags |= LLCelShadeVK::kFlagSpec;
    if (c.mIdsNow) p.flags |= LLCelShadeVK::kFlagIds;
    if (mSkinMetaSSBO.buffer) p.flags |= LLCelShadeVK::kFlagMeta;
    // Scope: whole scene / every avatar select all slots; otherwise the bitset, refreshed
    // every 15 frames (slot assignment, friends, avatars arriving) or at once on a change.
    if (c.mScope == LLCelShadeVK::SCOPE_SCENE) p.flags |= LLCelShadeVK::kFlagScene | LLCelShadeVK::kFlagAll;
    else if (c.mScope == LLCelShadeVK::SCOPE_AVATARS) p.flags |= LLCelShadeVK::kFlagAll;
    static uint32_t s_lastScope = ~0u, s_lastChosen = 0;
    const bool selWanted = (p.flags & (LLCelShadeVK::kFlagLight | LLCelShadeVK::kFlagIds)) != 0
                        && (p.flags & LLCelShadeVK::kFlagAll) == 0;
    if (selWanted && ((++c.mSelFrame % 15u) == 0u || s_lastScope != c.mScope || s_lastChosen != s_chosenGen))
    {
        s_lastScope = c.mScope;
        s_lastChosen = s_chosenGen;
        std::memset(p.sel, 0, sizeof(p.sel));
        LLCleanSkinStore& sk = LLCleanSkinStore::instance();
        auto mark = [&](const LLUUID& id)
        {
            const U32 slot = sk.findAvatarSlot(id);
            if (slot < kAvatarSlotCap) p.sel[slot >> 5] |= 1u << (slot & 31u);
        };
        if (c.mScope == LLCelShadeVK::SCOPE_SELF)
        {
            mark(gAgentID);
        }
        else if (c.mScope == LLCelShadeVK::SCOPE_CHOSEN)
        {
            for (const LLUUID& id : chosenAvatars()) mark(id);
        }
        else if (c.mScope == LLCelShadeVK::SCOPE_FRIENDS)
        {
            const LLAvatarTracker& at = LLAvatarTracker::instance();
            for (LLCharacter* ch : LLCharacter::sInstances)
            {
                LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(ch);
                if (av && !av->isDead() && at.isBuddy(av->getID())) mark(av->getID());
            }
        }
    }
    else if (!selWanted)
    {
        std::memset(p.sel, 0, sizeof(p.sel));
        s_lastScope = ~0u;
    }

    const bool changed = c.mParamsDirty || std::memcmp(&p, &c.mParams, sizeof(p)) != 0;
    if (changed)
    {
        c.mParams = p;
        c.mParamsDirty = false;
        // WAR vs the previous frame's shader reads (first scope = everything submitted
        // before on this queue), then the update, then visibility to every reader.
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(cmd, c.mParamsBuf.buffer, 0, sizeof(p), &p);
    }
    if (changed || (p.flags & (LLCelShadeVK::kFlagLight | LLCelShadeVK::kFlagIds)) != 0)
    {
        // Also covers the skin meta stream just above (its own barrier names compute only;
        // the world VS reads it for the avatar slot).
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                          | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
}

// Pipelines / layouts / sets, built on first use (immediate: recordWorld may run while a
// login pipeline batch is still live). false = unavailable (logged once).
static bool ensureCelPipelines(LLCelShadeVK& c, VkDevice dev,
                               const GraphicsPipelineDesc& litDesc, VkPipelineLayout drawLayout,
                               VkFormat hdrFormat)
{
    if (c.mIdPipesFailed) return false;
    if (c.mIdPipe[0] && c.mFilterPipe && c.mResolvePipe && c.mDebugPipe && c.mOutlinePipe && c.mHdrFormatBuilt == hdrFormat) return true;
    if (c.mDebugPipe && c.mHdrFormatBuilt != hdrFormat)
    {
        vkDeviceWaitIdle(dev);
        vkDestroyPipeline(dev, c.mDebugPipe, nullptr);
        c.mDebugPipe = VK_NULL_HANDLE;
        if (c.mOutlinePipe) vkDestroyPipeline(dev, c.mOutlinePipe, nullptr);
        c.mOutlinePipe = VK_NULL_HANDLE;
    }
    if (!c.mSampler)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &sci, nullptr, &c.mSampler) != VK_SUCCESS) c.mSampler = VK_NULL_HANDLE;
    }
    if (!c.mFilterDsl)
    {
        // Filter: 0 instances, 1 counters, 2..7 the six source lists, 8 output.
        VkDescriptorSetLayoutBinding fb[9]{};
        for (uint32_t i = 0; i < 9; ++i)
        {
            fb[i].binding = i; fb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            fb[i].descriptorCount = 1; fb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo fl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        fl.bindingCount = 9; fl.pBindings = fb;
        if (vkdCreateSetLayout(dev, &fl, &c.mFilterDsl, "mDslCelFilter") != VK_SUCCESS) c.mFilterDsl = VK_NULL_HANDLE;
        // Fullscreen consumers: 0 ids (usampler), 1 depth, 2 cel params.
        VkDescriptorSetLayoutBinding vb[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            vb[i].binding = i; vb[i].descriptorCount = 1; vb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            vb[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo vl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        vl.bindingCount = 3; vl.pBindings = vb;
        if (vkdCreateSetLayout(dev, &vl, &c.mViewDsl, "mDslCelView") != VK_SUCCESS) c.mViewDsl = VK_NULL_HANDLE;
        if (!c.mFilterDsl || !c.mViewDsl
            || c.mPool.create(dev, "cel", {{c.mFilterDsl, 2}, {c.mViewDsl, 2}}) != VK_SUCCESS
            || c.mPool.allocate(c.mFilterDsl, &c.mFilterSet[0]) != VK_SUCCESS
            || c.mPool.allocate(c.mFilterDsl, &c.mFilterSet[1]) != VK_SUCCESS
            || c.mPool.allocate(c.mViewDsl, &c.mViewSet[0]) != VK_SUCCESS
            || c.mPool.allocate(c.mViewDsl, &c.mViewSet[1]) != VK_SUCCESS)
        {
            LL_WARNS("VKCel") << "avatar mask descriptor sets unavailable; avatar mask / cel outlines disabled" << LL_ENDL;
            c.mIdPipesFailed = true;
            return false;
        }
        VkPushConstantRange fpr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FilterPush)};
        VkPipelineLayoutCreateInfo fli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        fli.setLayoutCount = 1; fli.pSetLayouts = &c.mFilterDsl;
        fli.pushConstantRangeCount = 1; fli.pPushConstantRanges = &fpr;
        if (vkdCreatePipelineLayout(dev, &fli, &c.mFilterLayout, "mCelFilterLayout") != VK_SUCCESS) c.mFilterLayout = VK_NULL_HANDLE;
        VkPushConstantRange vpr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkPipelineLayoutCreateInfo vli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        vli.setLayoutCount = 1; vli.pSetLayouts = &c.mViewDsl;
        vli.pushConstantRangeCount = 1; vli.pPushConstantRanges = &vpr;
        if (vkdCreatePipelineLayout(dev, &vli, &c.mViewLayout, "mCelViewLayout") != VK_SUCCESS) c.mViewLayout = VK_NULL_HANDLE;
    }
    if (!c.mFilterLayout || !c.mViewLayout || !c.mSampler)
    {
        c.mIdPipesFailed = true;
        LL_WARNS("VKCel") << "avatar mask layouts failed; avatar mask / cel outlines disabled" << LL_ENDL;
        return false;
    }
    if (!c.mFilterPipe)
    {
        VkShaderModule m = celModule(dev, kWorldCelFilterSpv, kWorldCelFilterSpv_bytes, "kWorldCelFilterSpv");
        if (m) { c.mFilterPipe = createComputePipeline(dev, m, c.mFilterLayout); vkDestroyShaderModule(dev, m, nullptr); }
    }
    if (!c.mIdPipe[0])
    {
        // The pass-1 lit pipelines' vertex input / formats / reversed-Z, retargeted to the
        // R16_UINT id attachment + read-only depth. Same VS modules (invariant gl_Position):
        // the EQUAL test holds against the pre-pass / pass-1 depth.
        VkShaderModule vs  = celModule(dev, kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
        VkShaderModule rvs = celModule(dev, kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes, "kWorldRiggedVertSpv");
        VkShaderModule fs  = celModule(dev, kWorldAvatarIdFragSpv, kWorldAvatarIdFragSpv_bytes, "kWorldAvatarIdFragSpv");
        VkShaderModule afs = celModule(dev, kWorldAvatarIdAlphaFragSpv, kWorldAvatarIdAlphaFragSpv_bytes, "kWorldAvatarIdAlphaFragSpv");
        if (vs && rvs && fs && afs)
        {
            GraphicsPipelineDesc d = litDesc;
            d.layout = drawLayout;
            d.colorFormat = LLCelShadeVK::kIdFormat;
            d.glowFormat = VK_FORMAT_UNDEFINED; d.glowWrite = false;
            d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
            d.mrtCount = 0;
            d.colorWrite = true; d.blend = false; d.additive = false; d.customBlend = false; d.glowAdditive = false;
            d.depthTest = true; d.depthWrite = false;
            d.fragSpec = nullptr; d.fsrAttachment = false; d.dynamicShadingRate = false;
            const VkCullModeFlags cm[3] = {VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_NONE, VK_CULL_MODE_NONE};
            for (int k = 0; k < 6; ++k)
            {
                const int t = k % 3;   // 0 back-culled, 1 double-sided, 2 alpha
                d.vert = k < 3 ? vs : rvs;
                if (k < 3) vkWorldStaticVI(d); else vkWorldRiggedVI(d);   // (#573)
                d.frag = t == 2 ? afs : fs;
                d.cullMode = cm[t];
                // (EQUAL stays exact under #1088's RenderVKPrepassEqualCompare: the opaque / mask
                // id FS has no alpha test -- EQUAL is what rejects a cut-out texel's pixel.)
                d.depthCompare = t == 2 ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_EQUAL;
                c.mIdPipe[k] = createGraphicsPipeline(dev, d);
            }
        }
        for (VkShaderModule m : {vs, rvs, fs, afs}) if (m) vkDestroyShaderModule(dev, m, nullptr);
    }
    if (!c.mResolvePipe || !c.mDebugPipe || !c.mOutlinePipe)
    {
        VkShaderModule fvs = celModule(dev, kWorldGlowCombineVertSpv, kWorldGlowCombineVertSpv_bytes, "kWorldGlowCombineVertSpv");
        VkShaderModule rfs = celModule(dev, kWorldAvatarIdResolveFragSpv, kWorldAvatarIdResolveFragSpv_bytes, "kWorldAvatarIdResolveFragSpv");
        VkShaderModule dfs = celModule(dev, kWorldAvatarIdDebugFragSpv, kWorldAvatarIdDebugFragSpv_bytes, "kWorldAvatarIdDebugFragSpv");
        VkShaderModule ofs = celModule(dev, kWorldCelOutlineFragSpv, kWorldCelOutlineFragSpv_bytes, "kWorldCelOutlineFragSpv");
        if (fvs && rfs && dfs && ofs)
        {
            GraphicsPipelineDesc d;
            d.vert = fvs; d.layout = c.mViewLayout;
            d.depthFormat = VK_FORMAT_UNDEFINED; d.depthTest = false; d.depthWrite = false;
            d.cullMode = VK_CULL_MODE_NONE;
            if (!c.mResolvePipe)
            {
                d.frag = rfs; d.colorFormat = LLCelShadeVK::kIdFormat; d.blend = false;
                c.mResolvePipe = createGraphicsPipeline(dev, d);
            }
            if (!c.mDebugPipe)
            {
                d.frag = dfs; d.colorFormat = hdrFormat; d.blend = true;
                c.mDebugPipe = createGraphicsPipeline(dev, d);
            }
            if (!c.mOutlinePipe)   // #550: src-alpha over the scene HDR
            {
                d.frag = ofs; d.colorFormat = hdrFormat; d.blend = true;
                c.mOutlinePipe = createGraphicsPipeline(dev, d);
            }
        }
        for (VkShaderModule m : {fvs, rfs, dfs, ofs}) if (m) vkDestroyShaderModule(dev, m, nullptr);
    }
    c.mHdrFormatBuilt = hdrFormat;
    bool ok = c.mFilterPipe && c.mResolvePipe && c.mDebugPipe && c.mOutlinePipe;
    for (VkPipeline p : c.mIdPipe) ok = ok && p;
    if (!ok)
    {
        LL_WARNS("VKCel") << "avatar mask pipelines failed to build; avatar mask / cel outlines disabled" << LL_ENDL;
        c.mIdPipesFailed = true;
        return false;
    }
    LL_INFOS("VKCel") << "avatar mask pipelines built" << LL_ENDL;
    return true;
}

void LLCleanRendererVK::recordCelPasses(VkCommandBuffer cmd, const VkDescriptorSet* dsets, const VKDrawPush& drawPush,
                                        const float* viewProjRelMain, bool do_rigged, uint32_t rw, uint32_t rh,
                                        uint32_t width, uint32_t height)
{
    if (!mCel || !mCtx) return;
    LLCelShadeVK& c = *mCel;
    VkDevice dev = mCtx->device();
    c.mMaskValid = false;

    if (!c.mIdsNow || !mDepth.view || rw == 0 || rh == 0)
    {
        // No consumer: free the targets (no idle VRAM cost). One wait, when it goes off.
        if (!c.mIdsNow && (c.mId.image || c.mIdDisp.image || c.mFilterBuf.buffer))
        {
            vkDeviceWaitIdle(dev);
            if (c.mId.image) mCtx->destroyImage(c.mId);
            if (c.mIdDisp.image) mCtx->destroyImage(c.mIdDisp);
            if (c.mFilterBuf.buffer) mCtx->destroyBuffer(c.mFilterBuf);
            c.mId = GpuImage{}; c.mIdDisp = GpuImage{}; c.mFilterBuf = GpuBuffer{};
            c.mIdW = c.mIdH = c.mIdDispW = c.mIdDispH = 0;
            c.mFilterCap = 0;
            ++c.mGen;
        }
        return;
    }
    if (!ensureCelPipelines(c, dev, mLitDesc, mDrawLayout, mHdrFormat)) return;

    // ---- targets (render extent; display copy only for the FX stack) ----
    const bool jittered = mTaaJitterOn;
    const bool wantDisp = c.mFxWantsMask && (mScaled || jittered || rw != width || rh != height);
    {
        VkMemScope mem(VKMEM_TARGET);   // #407
        const bool reId = !c.mId.image || c.mIdW != rw || c.mIdH != rh;
        const bool reDisp = wantDisp && (!c.mIdDisp.image || c.mIdDispW != width || c.mIdDispH != height);
        const bool dropDisp = !wantDisp && c.mIdDisp.image;
        if (reId || reDisp || dropDisp || !c.mFilterBuf.buffer)
        {
            vkDeviceWaitIdle(dev);   // the images / sets may be in use by frames in flight
            if (reId)
            {
                if (c.mId.image) mCtx->destroyImage(c.mId);
                c.mId = mCtx->createImage2D(rw, rh, LLCelShadeVK::kIdFormat,
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,   // <GMDX> avatar matte snapshots read it back
                                            VK_IMAGE_ASPECT_COLOR_BIT);
                c.mIdW = rw; c.mIdH = rh;
            }
            if (reDisp || dropDisp)
            {
                if (c.mIdDisp.image) mCtx->destroyImage(c.mIdDisp);
                c.mIdDisp = GpuImage{};
                c.mIdDispW = c.mIdDispH = 0;
                if (wantDisp)
                {
                    c.mIdDisp = mCtx->createImage2D(width, height, LLCelShadeVK::kIdFormat,
                                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                    VK_IMAGE_ASPECT_COLOR_BIT);
                    c.mIdDispW = width; c.mIdDispH = height;
                }
            }
            if (!c.mFilterBuf.buffer)
            {
                VkMemScope memc(VKMEM_CULL);
                c.mFilterCap = kFilterListCap;
                c.mFilterBuf = mCtx->createBuffer(kFilterHdr + 3 * (VkDeviceSize)kFilterListCap * kDrawCmdBytes,
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                                      | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            }
            ++c.mGen;
        }
    }
    if (!c.mId.image || !c.mFilterBuf.buffer || (wantDisp && !c.mIdDisp.image)) return;

    // ---- this frame slot's sets (the slot's previous use has retired: acquire waited it) ----
    const uint32_t slot = std::min<uint32_t>(mFrameSlot, 1u);
    const bool haveAlphaPost = mAlphaPost[0].buffer != VK_NULL_HANDLE;
    {
        const VkBuffer src[6] = {mCmd[0].buffer, mCmd[1].buffer, mCmd[6].buffer, mCmd[7].buffer, mCmd[2].buffer,
                                 haveAlphaPost ? mAlphaPost[0].buffer : mCmd[2].buffer};
        VkDescriptorBufferInfo bi[9];
        bi[0] = {mInstanceSSBO.buffer, 0, VK_WHOLE_SIZE};
        bi[1] = {mCounters.buffer, 0, VK_WHOLE_SIZE};
        for (int i = 0; i < 6; ++i) bi[2 + i] = {src[i], 0, VK_WHOLE_SIZE};
        bi[8] = {c.mFilterBuf.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[12]{};
        uint32_t n = 0;
        for (uint32_t i = 0; i < 9; ++i, ++n)
        {
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = c.mFilterSet[slot];
            w[n].dstBinding = i; w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &bi[i];
        }
        const VkDescriptorImageInfo ii[2] = {{c.mSampler, c.mId.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                             {c.mSampler, mDepth.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo pbi{c.mParamsBuf.buffer, 0, VK_WHOLE_SIZE};
        for (uint32_t i = 0; i < 3; ++i, ++n)
        {
            w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstSet = c.mViewSet[slot];
            w[n].dstBinding = i; w[n].descriptorCount = 1;
            if (i < 2) { w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &ii[i]; }
            else       { w[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[n].pBufferInfo = &pbi; }
        }
        vkUpdateDescriptorSets(dev, n, w, 0, nullptr);
    }

    // ---- worn rigid attachments: compact the static lists (clean_cel_filter.comp) ----
    // The cull / alpha sort wrote the lists in compute (pass 1 read them as indirect);
    // last frame's id pass read the output as indirect (WAR, first scope = prior work).
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, c.mFilterBuf.buffer, 0, kFilterHdr, 0u);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.mFilterPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.mFilterLayout, 0, 1, &c.mFilterSet[slot], 0, nullptr);
        const FilterPush fp{mBucketCap, c.mFilterCap, mInstanceCount, 0u};
        vkCmdPushConstants(cmd, c.mFilterLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fp), &fp);
        vkCmdDispatch(cmd, std::max<uint32_t>(1u, (mBucketCap + 63u) / 64u), 6u, 1u);
    }
    memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    gpuSubMark(cmd, "c_cel_filter");

    // ---- the id pass ----
    // Last frame's readers (outline / FX) are done with the old contents (cleared).
    imageBarrier(cmd, c.mId.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = c.mId.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.uint32[0] = 0u;
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = mDepth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {rw, rh}}; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0, 0, (float)rw, (float)rh, 0, 1};
        const VkRect2D sc{{0, 0}, {rw, rh}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        VkDeviceSize off = 0;
        bindDrawSets(cmd, dsets);
        // Worn rigid attachments (static VS, main view entry).
        pushDrawView(cmd, mDrawLayout, drawPush);
        vkCmdBindVertexBuffers(cmd, 0, 1, &mVbo.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mIbo.buffer, 0, VK_INDEX_TYPE_UINT16);
        for (uint32_t l = 0; l < 3; ++l)
        {
            bindGraphicsPipeline(cmd, c.mIdPipe[l]);
            vkCmdDrawIndexedIndirectCount(cmd, c.mFilterBuf.buffer, kFilterHdr + (VkDeviceSize)l * c.mFilterCap * kDrawCmdBytes,
                                          c.mFilterBuf.buffer, l * sizeof(uint32_t), c.mFilterCap, (uint32_t)kDrawCmdBytes);
        }
        // Rigged mesh + system bodies (pre-skinned, eye-relative; own avatar hidden as in
        // the main view: mouselook).
        if (do_rigged && mSkinVbo.buffer)
        {
            VKDrawPush rp = drawPush;
            std::memcpy(rp.view_proj, viewProjRelMain, sizeof(rp.view_proj));
            rp.ambient_color[3] = selfHideW();
            pushDrawView(cmd, mDrawLayout, rp);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mSkinVbo.buffer, &off);
            auto draw = [&](VkBuffer buf, uint32_t counter)
            {
                vkCmdDrawIndexedIndirectCount(cmd, buf, 0, mCounters.buffer, counter * sizeof(uint32_t),
                                              mBucketCap, (uint32_t)kDrawCmdBytes);
            };
            bindGraphicsPipeline(cmd, c.mIdPipe[3]);
            draw(mCmd[3].buffer, counterSlot(3));
            draw(mCmd[4].buffer, counterSlot(4));
            bindGraphicsPipeline(cmd, c.mIdPipe[4]);
            draw(mCmd[8].buffer, counterSlot(8));
            draw(mCmd[9].buffer, counterSlot(9));
            // Alpha-blended rigged (hair, sheer layers): pre-water bucket + post-water list
            // (counter slots 5 / 7, as pass 2 draws them).
            bindGraphicsPipeline(cmd, c.mIdPipe[5]);
            draw(mCmd[5].buffer, 5);
            if (mAlphaPost[1].buffer) draw(mAlphaPost[1].buffer, 7);
        }
        vkCmdEndRendering(cmd);
    }
    imageBarrier(cmd, c.mId.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gpuSubMark(cmd, "g_cel_ids");

    auto fullscreen = [&](VkImageView target, VkAttachmentLoadOp load, uint32_t w, uint32_t h, VkPipeline pipe,
                          const void* push, uint32_t pushBytes)
    {
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = target; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = load; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
        const VkRect2D sc{{0, 0}, {w, h}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        bindGraphicsPipeline(cmd, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.mViewLayout, 0, 1, &c.mViewSet[slot], 0, nullptr);
        if (push && pushBytes) vkCmdPushConstants(cmd, c.mViewLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushBytes, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };

    // ---- FX mask: display-res, unjittered (or the render target itself) ----
    if (c.mFxWantsMask)
    {
        if (wantDisp)
        {
            imageBarrier(cmd, c.mIdDisp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            ResolvePush rp{};
            rp.dsize[0] = (int32_t)width; rp.dsize[1] = (int32_t)height;
            rp.rsize[0] = (int32_t)rw;    rp.rsize[1] = (int32_t)rh;
            rp.ratio[0] = (float)rw / (float)width; rp.ratio[1] = (float)rh / (float)height;
            rp.jitter[0] = jittered ? mTaaJitter[0] : 0.f;
            rp.jitter[1] = jittered ? mTaaJitter[1] : 0.f;
            fullscreen(c.mIdDisp.view, VK_ATTACHMENT_LOAD_OP_DONT_CARE, width, height, c.mResolvePipe, &rp, sizeof(rp));
            imageBarrier(cmd, c.mIdDisp.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            c.mMaskIsRender = false;
            gpuSubMark(cmd, "g_cel_mask_resolve");
        }
        else
        {
            c.mMaskIsRender = true;   // render extent == display extent, no jitter
        }
        c.mMaskValid = true;
    }

    // ---- scene-space consumers (render res, into the scene HDR before pass 2 / TAA) ----
    bool drewScene = false;
    if (c.mOutlineNow)
    {
        // Width: pixels at a 1080-line output, scaled to this output's height (live view and
        // large snapshots look alike), then to the render extent; radius = half (both
        // sides of an edge are tested, the line lands on one).
        OutlinePush op{};
        op.dims[0] = (float)rw; op.dims[1] = (float)rh; op.dims[2] = 1.f / (float)rw; op.dims[3] = 1.f / (float)rh;
        op.lin[0] = mCurInvProj[10]; op.lin[1] = mCurInvProj[14]; op.lin[2] = mCurInvProj[11]; op.lin[3] = mCurInvProj[15];
        for (int i = 0; i < 4; ++i) op.color[i] = c.mOutlineColor[i];
        const float wDisp = c.mOutlineWidth * (float)height / 1080.f;
        op.p0[0] = std::max(0.5f, 0.5f * wDisp * (float)rh / (float)height);
        op.p0[1] = c.mOutlineDepth;
        op.p0[2] = c.mOutlineCrease;
        op.p0[3] = c.mScope == LLCelShadeVK::SCOPE_SCENE ? 0.f : 1.f;
        op.p1[0] = 0.5f * c.mOutlineFade; op.p1[1] = c.mOutlineFade;
        fullscreen(sceneHdr().view, VK_ATTACHMENT_LOAD_OP_LOAD, rw, rh, c.mOutlinePipe, &op, sizeof(op));
        drewScene = true;
    }
    if (c.mDebugNow)
    {
        fullscreen(sceneHdr().view, VK_ATTACHMENT_LOAD_OP_LOAD, rw, rh, c.mDebugPipe, nullptr, 0);
        drewScene = true;
    }
    if (drewScene)
    {
        // Pass 2 blends over the scene HDR again (and the god-ray composite reads nothing of it).
        memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        gpuSubMark(cmd, "g_cel_scene");
    }
}
