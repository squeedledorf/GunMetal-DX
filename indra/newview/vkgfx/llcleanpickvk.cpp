/**
 * @file llcleanpickvk.cpp
 * @brief GPU raster cursor pick (#702 / #704) and particle pick (#481). See llcleanpickvk.h.
 */
#include "linden_common.h"

#include "llcleanpickvk.h"
#include "llcleanrenderervk.h"
#include "llcleanclustervk.h"   // cluster LOD (#702 integration): the pick of cluster-drawn objects
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"
#include "lltimer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
// clean_pick_select.comp PickSelectPush (112 B).
struct PickSelectPush
{
    float    w_org[4];   // world ray origin (agent space), w length
    float    w_dir[4];   // unit direction
    float    h_org[4];   // HUD ray origin (HUD space), w length
    float    h_dir[4];   // unit direction
    uint32_t job[4];     // instance rows, world list cap, HUD list cap, bit0 world / bit1 HUD
    uint32_t valid[4];   // index / vertex extents (OOB guards; 0 = unchecked), snapshot rows
    float    win[4];     // depth windows: world [lo, hi), HUD [lo, hi)
};
static_assert(sizeof(PickSelectPush) == 112 && sizeof(PickSelectPush) <= kVkPushConstantBudget, "PickSelectPush");

// clean_pick.frag pad1 bits (the draw push's last word).
constexpr uint32_t kPadLayerHud = 1u;   // PICK_LAYER_HUD (the request's MODE_* bits ride above it)

// clean_pick_particle.comp PartPickPush (96 B).
struct PartPickPush
{
    float    origin[4];   // xyz segment start, w length
    float    dir[4];      // xyz unit direction, w alpha threshold (< 0: no texture test)
    float    right[4];    // camera basis the billboards are built on (clean_particle.vert)
    float    up[4];
    float    fwd[4];
    uint32_t hdr[4];      // serial, entry, pass, pool size
};
static_assert(sizeof(PartPickPush) == 96, "PartPickPush");

constexpr uint32_t kInstanceBytes = 224;   // clean Instance row

VkShaderModule pickModule(VkDevice dev, const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

void computeBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcA,
                    VkPipelineStageFlags2 dst, VkAccessFlags2 dstA)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = src; mb.srcAccessMask = srcA;
    mb.dstStageMask = dst; mb.dstAccessMask = dstA;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

// PickHdr (clean_pick_common.glsl) byte offsets.
constexpr size_t kHdrSerial = 0, kHdrNWorld = 4, kHdrNHud = 8, kHdrMode = 12, kHdrSel = 16,
                 kHdrWOrg = 32, kHdrWDir = 48, kHdrHOrg = 64, kHdrHDir = 80, kHdrWin = 96,
                 kHdrHistW = 112, kHdrHistH = 112 + 4 * LLCleanPickVK::kHistWorld;
static_assert(kHdrHistH + 4 * LLCleanPickVK::kHistHud + 16 == LLCleanPickVK::kHdrBytes, "PickHdr layout");
} // namespace

LLCleanPickVK::LLCleanPickVK(LLCleanRendererVK& r) : mR(r)
{
}

uint32_t LLCleanPickVK::slotIndex() const
{
    return mR.mFrameSlot % kSlots;
}

float LLCleanPickVK::binEdge(uint32_t b, uint32_t nb, float lo, float hi)
{
    // pickBin: u = log2(1 + (t - lo)) / log2(1 + span), bin = floor(u * nb).
    const float span = std::max(std::min(hi, 1.0e6f) - lo, 1.0e-3f);
    if (b >= nb) return hi;
    const float u = (float)b / (float)nb;
    return lo + std::exp2(u * std::log2(1.f + span)) - 1.f;
}

void LLCleanPickVK::destroy(VkDevice dev)
{
    if (mSelPipe) vkDestroyPipeline(dev, mSelPipe, nullptr);
    if (mStaticPipe) vkDestroyPipeline(dev, mStaticPipe, nullptr);
    if (mRigPipe) vkDestroyPipeline(dev, mRigPipe, nullptr);
    if (mWinPipe) vkDestroyPipeline(dev, mWinPipe, nullptr);
    if (mSelLayout) vkDestroyPipelineLayout(dev, mSelLayout, nullptr);
    if (mDrawLayout) vkDestroyPipelineLayout(dev, mDrawLayout, nullptr);
    if (mWinLayout) vkDestroyPipelineLayout(dev, mWinLayout, nullptr);
    mSelPipe = mStaticPipe = mRigPipe = mWinPipe = VK_NULL_HANDLE;
    mSelLayout = mDrawLayout = mWinLayout = VK_NULL_HANDLE;
    mSelLayoutDsl = VK_NULL_HANDLE;
    mDrawLayoutDsl[0] = mDrawLayoutDsl[1] = VK_NULL_HANDLE;
    mPool.destroy();
    if (mDslSel) vkdDestroySetLayout(dev, mDslSel);
    if (mDslOut) vkdDestroySetLayout(dev, mDslOut);
    if (mDslWin) vkdDestroySetLayout(dev, mDslWin);
    mDslSel = mDslOut = mDslWin = VK_NULL_HANDLE;
    mSetOut = mSetWin = VK_NULL_HANDLE;
    for (VkDescriptorSet& s : mSetSel) s = VK_NULL_HANDLE;
    if (mCtx)
    {
        if (mOut.buffer) mCtx->destroyBuffer(mOut);
        if (mLists.buffer) mCtx->destroyBuffer(mLists);
        if (mTarget.image) mCtx->destroyImage(mTarget);
        if (mSnapCmd.buffer) mCtx->destroyBuffer(mSnapCmd);
        if (mSnapMeta.buffer) mCtx->destroyBuffer(mSnapMeta);
    }
    mOut = GpuBuffer{};
    mLists = GpuBuffer{};
    mTarget = GpuImage{};
    mSnapCmd = GpuBuffer{};
    mSnapMeta = GpuBuffer{};
    mSnapCap = mSnapRows = 0;
    for (SlotState& s : mSlot) s.pending = false;
    mFrame.selected = false;
    // Drawn-geometry queries (#704).
    if (mQueryPipe) vkDestroyPipeline(dev, mQueryPipe, nullptr);
    if (mQueryLayout) vkDestroyPipelineLayout(dev, mQueryLayout, nullptr);
    mQueryPipe = VK_NULL_HANDLE;
    mQueryLayout = VK_NULL_HANDLE;
    mQueryLayoutDsl = VK_NULL_HANDLE;
    mQueryPool.destroy();
    if (mDslQuery) vkdDestroySetLayout(dev, mDslQuery);
    mDslQuery = VK_NULL_HANDLE;
    mSetQuery = VK_NULL_HANDLE;
    if (mCtx)
    {
        if (mQueryIn.buffer) mCtx->destroyBuffer(mQueryIn);
        if (mQueryOut.buffer) mCtx->destroyBuffer(mQueryOut);
    }
    mQueryIn = GpuBuffer{};
    mQueryOut = GpuBuffer{};
    mQueryCap = 0;
    // Particle pick (#481).
    if (mPartPipe) vkDestroyPipeline(dev, mPartPipe, nullptr);
    if (mPartLayout) vkDestroyPipelineLayout(dev, mPartLayout, nullptr);
    mPartPipe = VK_NULL_HANDLE;
    mPartLayout = VK_NULL_HANDLE;
    mPartDescPool.destroy();
    if (mDslPart) vkdDestroySetLayout(dev, mDslPart);
    mDslPart = VK_NULL_HANDLE;
    for (VkDescriptorSet& s : mSetPart) s = VK_NULL_HANDLE;
    if (mCtx)
    {
        if (mPartOut.buffer) mCtx->destroyBuffer(mPartOut);
        for (GpuBuffer& b : mPartPartial) if (b.buffer) mCtx->destroyBuffer(b);
    }
    mPartOut = GpuBuffer{};
    for (GpuBuffer& b : mPartPartial) b = GpuBuffer{};
    mPartGroups = 0;
    mCtx = nullptr;
}

bool LLCleanPickVK::available() const
{
    // (fragmentStoresAndAtomics is a device requirement since #704.)
    return mR.mCtx && !mBaseFailed && !mSelFailed && !mDrawFailed;
}

const char* LLCleanPickVK::failure() const
{
    if (!mR.mCtx) return "no device context";
    if (mBaseFailed) return "result buffers / target allocation failed (latched)";
    if (mSelFailed) return "select pipeline creation failed (latched)";
    if (mDrawFailed) return "draw pipeline creation failed (latched)";
    return mNowFail;
}

// Result entries, candidate lists, the 1x1 target, the two set layouts and the pool (the
// pipelines come on first use).
bool LLCleanPickVK::ensureBase()
{
    VkMemScope memcat(VKMEM_CULL);   // #407 (cursor pick)
    if (mSetOut) return true;
    if (mBaseFailed || !mR.mCtx) return false;
    mCtx = mR.mCtx;
    VkDevice dev = mCtx->device();
    mOut = mCtx->createBuffer((VkDeviceSize)kEntries * kEntryBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mLists = mCtx->createBuffer((VkDeviceSize)kListHdrBytes + (VkDeviceSize)(2u * kListCap + kHudListCap) * kDrawCmdBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                                | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mTarget = mCtx->createImage2D(1, 1, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!mOut.buffer || !mOut.mapped || !mLists.buffer || !mTarget.image || !mTarget.view)
    {
        // #698 policy: no crash -- the pick answers nothing until a later retry succeeds.
        LL_WARNS("Vulkan") << "cursor pick: buffer / target allocation failed; no object picking" << LL_ENDL;
        if (mOut.buffer) mCtx->destroyBuffer(mOut);
        if (mLists.buffer) mCtx->destroyBuffer(mLists);
        if (mTarget.image) mCtx->destroyImage(mTarget);
        mOut = GpuBuffer{}; mLists = GpuBuffer{}; mTarget = GpuImage{};
        mBaseFailed = true;
        return false;
    }
    std::memset(mOut.mapped, 0, (size_t)mOut.size);   // serial 0 = never answered

    // Select set: 0 snapshot cmd, 1 snapshot meta, 2 slots, 3 LOD groups, 4 av_vis, 5 skin meta, 6 lists.
    VkDescriptorSetLayoutBinding sb[7]{};
    for (uint32_t i = 0; i < 7; ++i)
    {
        sb[i].binding = i; sb[i].descriptorCount = 1; sb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        sb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 7; sl.pBindings = sb;
    VK_CHECK(vkdCreateSetLayout(dev, &sl, &mDslSel, "mDslPickSel"));
    // Draw set 2: the entries (clean_pick.frag appends).
    VkDescriptorSetLayoutBinding ob{};
    ob.binding = 0; ob.descriptorCount = 1; ob.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ob.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo ol{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ol.bindingCount = 1; ol.pBindings = &ob;
    VK_CHECK(vkdCreateSetLayout(dev, &ol, &mDslOut, "mDslPickOut"));
    // Count-then-fill set: 0 the entries, 1 the lists' counters.
    VkDescriptorSetLayoutBinding wb[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        wb[i].binding = i; wb[i].descriptorCount = 1; wb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        wb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo wl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    wl.bindingCount = 2; wl.pBindings = wb;
    VK_CHECK(vkdCreateSetLayout(dev, &wl, &mDslWin, "mDslPickWin"));
    VK_CHECK(mPool.create(dev, "pick", {{mDslOut, 1}, {mDslSel, kEntries}, {mDslWin, 1}}));
    VKD_ALLOC_SOFT(mPool.allocate(mDslOut, &mSetOut), { mBaseFailed = true; return false; });
    VKD_ALLOC_SOFT(mPool.allocate(mDslWin, &mSetWin), { mWinFailed = true; });
    for (uint32_t i = 0; i < kEntries; ++i) VKD_ALLOC_SOFT(mPool.allocate(mDslSel, &mSetSel[i]), { mBaseFailed = true; return false; });
    if (!mSetOut)
    {
        mBaseFailed = true;
        return false;
    }
    VkDescriptorBufferInfo bi{mOut.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = mSetOut; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    if (mSetWin)
    {
        const VkDescriptorBufferInfo wbi[2] = {{mOut.buffer, 0, VK_WHOLE_SIZE}, {mLists.buffer, 0, kListHdrBytes}};
        VkWriteDescriptorSet ww[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            ww[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; ww[i].dstSet = mSetWin;
            ww[i].dstBinding = i; ww[i].descriptorCount = 1;
            ww[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ww[i].pBufferInfo = &wbi[i];
        }
        vkUpdateDescriptorSets(dev, 2, ww, 0, nullptr);
    }
    return true;
}

// The count-then-fill pipeline (no draw-set dependency: built once). Without it an
// overflowing answer reaches the CPU, which narrows the window with a synchronous pick.
bool LLCleanPickVK::ensureWindow()
{
    if (mWinPipe) return true;
    if (mWinFailed || !ensureBase() || !mSetWin) return false;
    VkDevice dev = mCtx->device();
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(uint32_t)};   // + the cluster pick list (#702)
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1; li.pSetLayouts = &mDslWin;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mWinLayout, "mPickWinLayout") != VK_SUCCESS)
    {
        mWinLayout = VK_NULL_HANDLE;
        mWinFailed = true;
        return false;
    }
    VkShaderModule m = pickModule(dev, kWorldPickWindowSpv, kWorldPickWindowSpv_bytes);
    mWinPipe = m ? createComputePipeline(dev, m, mWinLayout) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mWinPipe)
    {
        LL_WARNS("Vulkan") << "cursor pick: count-then-fill pipeline creation failed (overflowing picks narrow on the CPU)" << LL_ENDL;
        mWinFailed = true;
        return false;
    }
    return true;
}

// The select pipeline over {draw set 0, the select set} (rebuilt if draw set 0's layout changed).
bool LLCleanPickVK::ensureSelect()
{
    if (mSelFailed || !ensureBase()) return false;
    const VkDescriptorSetLayout want = mR.mDslDraw0;
    if (mSelPipe && mSelLayoutDsl == want) return true;
    if (!want) return false;
    VkDevice dev = mCtx->device();
    if (mSelPipe || mSelLayout)
    {
        vkDeviceWaitIdle(dev);   // a set layout changed under it (rare): rebuild
        if (mSelPipe) vkDestroyPipeline(dev, mSelPipe, nullptr);
        if (mSelLayout) vkDestroyPipelineLayout(dev, mSelLayout, nullptr);
        mSelPipe = VK_NULL_HANDLE; mSelLayout = VK_NULL_HANDLE;
    }
    const VkDescriptorSetLayout sets[2] = {want, mDslSel};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PickSelectPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2; li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mSelLayout, "mPickSelLayout") != VK_SUCCESS)
    {
        mSelLayout = VK_NULL_HANDLE;
        mSelFailed = true;
        return false;
    }
    VkShaderModule m = pickModule(dev, kWorldPickSelectSpv, kWorldPickSelectSpv_bytes);
    mSelPipe = m ? createComputePipeline(dev, m, mSelLayout) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mSelPipe)
    {
        LL_WARNS("Vulkan") << "cursor pick: select pipeline creation failed; no object picking" << LL_ENDL;
        mSelFailed = true;
        return false;
    }
    mSelLayoutDsl = want;
    return true;
}

// The two 1x1 draw pipelines: the world VS modules (static / rigged) + clean_pick.frag,
// over {draw set 0, draw set 1, the entries} with the draw family's 16 B push range (so
// sets 0 / 1 stay compatible with the passes around it). Cull none, no depth, the colour
// write masked off: every fragment reaches the FS.
bool LLCleanPickVK::ensureDraw()
{
    if (mDrawFailed || !ensureBase()) return false;
    const VkDescriptorSetLayout want[2] = {mR.mDslDraw0, mR.mDslDraw1};
    if (mStaticPipe && mRigPipe && std::equal(want, want + 2, mDrawLayoutDsl)) return true;
    if (!want[0] || !want[1]) return false;
    VkDevice dev = mCtx->device();
    if (mStaticPipe || mRigPipe || mDrawLayout)
    {
        vkDeviceWaitIdle(dev);   // a set layout changed under it (rare): rebuild
        if (mStaticPipe) vkDestroyPipeline(dev, mStaticPipe, nullptr);
        if (mRigPipe) vkDestroyPipeline(dev, mRigPipe, nullptr);
        if (mDrawLayout) vkDestroyPipelineLayout(dev, mDrawLayout, nullptr);
        mStaticPipe = mRigPipe = VK_NULL_HANDLE; mDrawLayout = VK_NULL_HANDLE;
    }
    const VkDescriptorSetLayout sets[3] = {want[0], want[1], mDslOut};
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VKDrawViewPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 3; li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mDrawLayout, "mPickDrawLayout") != VK_SUCCESS)
    {
        mDrawLayout = VK_NULL_HANDLE;
        mDrawFailed = true;
        return false;
    }
    VkShaderModule vs = pickModule(dev, kWorldVertSpv, kWorldVertSpv_bytes);
    VkShaderModule rvs = pickModule(dev, kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes);
    VkShaderModule fs = pickModule(dev, kWorldPickFragSpv, kWorldPickFragSpv_bytes);
    if (vs && rvs && fs)
    {
        GraphicsPipelineDesc d;
        d.layout = mDrawLayout;
        d.frag = fs;
        d.colorFormat = VK_FORMAT_R8_UNORM;
        d.colorWrite = false;
        d.depthFormat = VK_FORMAT_UNDEFINED;
        d.depthTest = false; d.depthWrite = false; d.blend = false;
        d.cullMode = VK_CULL_MODE_NONE;   // facing decided in the FS (gl_FrontFacing)
        d.vert = vs; vkWorldStaticVI(d);
        mStaticPipe = createGraphicsPipeline(dev, d);
        d.vert = rvs; vkWorldRiggedVI(d);
        mRigPipe = createGraphicsPipeline(dev, d);
    }
    if (vs) vkDestroyShaderModule(dev, vs, nullptr);
    if (rvs) vkDestroyShaderModule(dev, rvs, nullptr);
    if (fs) vkDestroyShaderModule(dev, fs, nullptr);
    if (!mStaticPipe || !mRigPipe)
    {
        LL_WARNS("Vulkan") << "cursor pick: draw pipeline creation failed; no object picking" << LL_ENDL;
        mDrawFailed = true;
        return false;
    }
    std::copy(want, want + 2, mDrawLayoutDsl);
    return true;
}

// The snapshot buffers for `rows` instance rows (grown 1.5x; the old pair is retired with
// the frame, its last readers are earlier frames).
bool LLCleanPickVK::ensureSnapshot(uint32_t rows)
{
    if (rows <= mSnapCap && mSnapCmd.buffer && mSnapMeta.buffer) return true;
    VkMemScope memcat(VKMEM_CULL);   // #407 (cursor pick)
    const uint32_t cap = std::max<uint32_t>(rows + rows / 2u, 4096u);
    GpuBuffer cmd = mCtx->createBuffer((VkDeviceSize)cap * kDrawCmdBytes,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    GpuBuffer meta = mCtx->createBuffer((VkDeviceSize)cap * sizeof(uint32_t),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!cmd.buffer || !meta.buffer)
    {
        // #698: keep the old snapshot (picks see only the rows it holds); retry next frame.
        if (cmd.buffer) mCtx->destroyBuffer(cmd);
        if (meta.buffer) mCtx->destroyBuffer(meta);
        static bool s_warned = false;
        if (!s_warned)
        {
            s_warned = true;
            LL_WARNS("Vulkan") << "cursor pick: snapshot allocation for " << rows << " rows failed; retrying" << LL_ENDL;
        }
        return mSnapCmd.buffer && mSnapMeta.buffer;
    }
    mR.retireBuffer(mSnapCmd);
    mR.retireBuffer(mSnapMeta);
    mSnapCmd = cmd;
    mSnapMeta = meta;
    mSnapCap = cap;
    mSnapRows = 0;
    return true;
}

void LLCleanPickVK::hostBarrier(VkCommandBuffer cmd)
{
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
}

void LLCleanPickVK::readEntry(uint32_t entry, const LLVKPickRequest& req, LLVKPickResult& out) const
{
    out = LLVKPickResult{};
    if (!mOut.mapped) return;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mOut.mapped) + (size_t)entry * kEntryBytes;
    uint32_t serial = 0, nw = 0, nh = 0, sel[4] = {};
    std::memcpy(&serial, base + kHdrSerial, 4);
    if (serial != req.serial || serial == 0u) return;   // not this request's answer (never completed / stale)
    std::memcpy(&nw, base + kHdrNWorld, 4);
    std::memcpy(&nh, base + kHdrNHud, 4);
    std::memcpy(sel, base + kHdrSel, sizeof(sel));
    out.valid = true;
    out.req = req;
    out.cand_static = sel[0];
    out.cand_rigged = sel[1];
    out.cand_hud = sel[2];
    out.cand_overflow_world = (sel[3] & 1u) != 0u;    // clean_pick_select.comp bits 1 static,
    out.cand_overflow_rigged = (sel[3] & 2u) != 0u;   // 2 rigged (a partial rigged layer, see
    out.cand_overflow_hud = (sel[3] & 4u) != 0u;      // LLVKPickResult), 4 HUD
    out.n_world = nw;
    out.n_hud = nh;
    out.overflow_world = nw > kWorldFrags;
    out.overflow_hud = nh > kHudFrags;
    std::memcpy(out.win, base + kHdrWin, sizeof(out.win));
    std::memcpy(out.hist_world, base + kHdrHistW, sizeof(out.hist_world));
    std::memcpy(out.hist_hud, base + kHdrHistH, sizeof(out.hist_hud));
    auto parse = [&](const uint8_t* f, std::vector<LLVKPickFrag>& v, uint32_t n)
    {
        v.resize(n);
        for (uint32_t i = 0; i < n; ++i, f += kFragBytes)
        {
            uint32_t ids[4];
            float pos_t[4], nrm[4], tng[4], st[4], duv[4];
            std::memcpy(ids, f, 16);
            std::memcpy(pos_t, f + 16, 16);
            std::memcpy(nrm, f + 32, 16);
            std::memcpy(tng, f + 48, 16);
            std::memcpy(st, f + 64, 16);
            std::memcpy(duv, f + 80, 16);
            LLVKPickFrag& o = v[i];
            o.instance = ids[0];
            o.flags = ids[1];
            std::memcpy(&o.alpha, &ids[2], 4);
            o.t = pos_t[3];
            std::memcpy(o.pos, pos_t, 3 * sizeof(float));
            std::memcpy(o.normal, nrm, 3 * sizeof(float));
            std::memcpy(o.tangent, tng, 4 * sizeof(float));
            o.st[0] = st[0]; o.st[1] = st[1];
            o.uv[0] = st[2]; o.uv[1] = st[3];
            std::memcpy(o.duv, duv, 4 * sizeof(float));
        }
    };
    // An overflowing layer's stored fragments are an arbitrary subset: not handed out
    // (the CPU narrows the window from the histogram instead).
    if (!out.overflow_world) parse(base + kHdrBytes, out.world, nw);
    if (!out.overflow_hud) parse(base + kHdrBytes + (size_t)kWorldFrags * kFragBytes, out.hud, nh);
}

void LLCleanPickVK::harvest(uint32_t s)
{
    SlotState& st = mSlot[s];
    if (!st.pending) return;
    st.pending = false;
    LLVKPickResult r;
    readEntry(s, st.req, r);
    if (r.valid && (!mLatest.valid || (int32_t)(r.req.serial - mLatest.req.serial) > 0)) mLatest = std::move(r);
}

void LLCleanPickVK::beginFrame()
{
    mFrame.selected = false;
    const uint32_t s = slotIndex();
    // Particle pick (#481): this slot's previous answer, when its frame recorded one.
    PartSlotState& ps = mPartSlot[s];
    if (ps.pending)
    {
        LLVKPartPickResult pr;
        readPartEntry(s, ps.req, pr);
        ps.pending = false;
        if (pr.valid && (!mPartLatest.valid || (int32_t)(pr.serial - mPartLatest.serial) > 0)) mPartLatest = pr;
    }
    harvest(s);
    // <SS:Skoomastream> this slot's aux answers
    for (uint32_t k = 0; k < kAux; ++k)
    {
        SlotState& st = mAuxSlot[s][k];
        if (!st.pending) continue;
        st.pending = false;
        LLVKPickResult r;
        readEntry(kSlots + 1 + s * kAux + k, st.req, r);
        if (!r.valid) continue;
        if (mAuxDone.size() >= 4 * kAux) mAuxDone.erase(mAuxDone.begin());   // nobody is taking them
        mAuxDone.push_back(std::move(r));
    }
    // </SS:Skoomastream>
}

// <SS:Skoomastream>
void LLCleanPickVK::setAuxRequests(const LLVKPickRequest* reqs)
{
    for (uint32_t k = 0; k < kAux; ++k) mAuxReq[k] = reqs[k];
}

void LLCleanPickVK::takeAux(std::vector<LLVKPickResult>& out)
{
    for (LLVKPickResult& r : mAuxDone) out.push_back(std::move(r));
    mAuxDone.clear();
}

// After the cursor ray's draw: each aux ray's select + 1x1 draws into its slot entry,
// through a projection about its own origin (the request's world_vp / apex).
void LLCleanPickVK::recordAux(VkCommandBuffer cmd)
{
    bool any = false;
    for (const LLVKPickRequest& rq : mAuxReq) any = any || rq.valid;
    if (!any || !mSnapRows) return;
    if (!ensureDraw() || !mR.mDsetDraw0 || !mR.mDsetDraw1 || !mR.mVbo.buffer || !mR.mIbo.buffer) return;
    const uint32_t s = slotIndex();
    for (uint32_t k = 0; k < kAux; ++k)
    {
        SlotState& st = mAuxSlot[s][k];
        st.pending = false;
        const LLVKPickRequest& rq = mAuxReq[k];
        if (!rq.valid || rq.hud) continue;
        // (This slot's previous frame completed: its fence was waited, and its answers were
        // harvested in beginFrame.)
        const uint32_t entry = kSlots + 1 + s * kAux + k;
        st.req = rq;
        // <GMDX> Aux rays don't run the cluster pick: its filter has one output list per frame,
        // which the main pick owns. (An unset entry read garbage and called it on a null cluster.)
        mClPick[entry] = false;
        writeHeader(entry, rq);
        if (!recordSelectInto(cmd, rq, entry)) continue;
        VKDrawPush wv, hv;
        drawViews(rq, mSnapCam, mSnapHide, wv, hv);
        const uint32_t view_w = mR.ringAlloc(mR.mViewRgn, &wv, (uint32_t)sizeof(VKDrawPush));
        recordDrawInto(cmd, rq, entry, view_w, 0u);
        st.pending = true;
    }
}
// </SS:Skoomastream>

bool LLCleanPickVK::latest(LLVKPickResult& out) const
{
    if (!mLatest.valid) return false;
    out = mLatest;
    return true;
}

// The entry's header for req (host-visible; the entry's previous use has completed):
// counts and histograms zeroed, the GPU writes the serial last.
void LLCleanPickVK::writeHeader(uint32_t entry, const LLVKPickRequest& req)
{
    uint8_t* base = reinterpret_cast<uint8_t*>(mOut.mapped) + (size_t)entry * kEntryBytes;
    std::memset(base, 0, kHdrBytes);
    std::memcpy(base + kHdrMode, &req.mode, 4);
    const float w_org[4] = {req.origin[0], req.origin[1], req.origin[2], req.length};
    const float w_dir[4] = {req.dir[0], req.dir[1], req.dir[2], req.alpha_threshold};
    const float h_org[4] = {req.hud_origin[0], req.hud_origin[1], req.hud_origin[2], req.hud_length};
    const float h_dir[4] = {req.hud_dir[0], req.hud_dir[1], req.hud_dir[2], 0.f};
    std::memcpy(base + kHdrWOrg, w_org, 16);
    std::memcpy(base + kHdrWDir, w_dir, 16);
    std::memcpy(base + kHdrHOrg, h_org, 16);
    std::memcpy(base + kHdrHDir, h_dir, 16);
    std::memcpy(base + kHdrWin, req.win, 16);
}

// Every main frame: the main cull's cand[] / cand_meta[] rows into the snapshot.
void LLCleanPickVK::recordSnapshot(VkCommandBuffer cmd, const float cam_time[4], float hide_self_w)
{
    if (!mR.mCand.buffer || !mR.mCandMeta.buffer || !ensureBase()) return;
    uint32_t rows = (uint32_t)(mR.mCpuInstances.size() / kInstanceBytes);
    rows = std::min<uint32_t>(rows, (uint32_t)(mR.mCand.size / kDrawCmdBytes));
    rows = std::min<uint32_t>(rows, (uint32_t)(mR.mCandMeta.size / sizeof(uint32_t)));
    if (!rows || !ensureSnapshot(rows)) return;
    if (rows > mSnapCap)
    {
        // The snapshot could not grow (allocation failed; retried every frame): rows past its
        // capacity are not pickable -- the newest objects can't be clicked. Always on, rate-limited.
        static F64 s_clip_next = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now >= s_clip_next)
        {
            s_clip_next = now + 30.0;
            LL_INFOS("VKPick") << "pick health: snapshot holds " << mSnapCap << " of " << rows
                               << " instance rows (growth allocation failing); objects in the rows past it can't be picked"
                               << LL_ENDL;
        }
    }
    rows = std::min(rows, mSnapCap);
    // The main cull / pre-skin writes of cand[] -> the copy; earlier reads of the snapshot
    // (the previous frame's select, a synchronous pick) -> its overwrite (WAR).
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const VkBufferCopy cc{0, 0, (VkDeviceSize)rows * kDrawCmdBytes};
    vkCmdCopyBuffer(cmd, mR.mCand.buffer, mSnapCmd.buffer, 1, &cc);
    const VkBufferCopy cm{0, 0, (VkDeviceSize)rows * sizeof(uint32_t)};
    vkCmdCopyBuffer(cmd, mR.mCandMeta.buffer, mSnapMeta.buffer, 1, &cm);
    // The copy reads cand[] before the HUD / offscreen culls rewrite it (WAR), and its
    // writes -> the select below.
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                   | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    mSnapRows = rows;
    std::memcpy(mSnapCam, cam_time, sizeof(mSnapCam));
    mSnapHide = hide_self_w;
}

bool LLCleanPickVK::recordSelectInto(VkCommandBuffer cmd, const LLVKPickRequest& req, uint32_t entry)
{
    if (!ensureSelect() || !mSnapRows) return false;
    if (!mSnapCmd.buffer || !mSnapMeta.buffer || !mR.mSlotSSBO.buffer || !mR.mLodGroupSSBO.buffer
        || !mR.mAvVisSSBO.buffer || !mR.mDsetDraw0)
        return false;
    // No skin meta buffer yet (no rigged mesh seen this session): binding 5 aliases av_vis,
    // as the cull's sets do -- av_vis[AV_VIS_SLOTS] is 0 then, so the select never reads it
    // (requiring it left a session without rigged mesh unable to pick anything).
    const VkBuffer skin_meta = mR.mSkinMetaSSBO.buffer ? mR.mSkinMetaSSBO.buffer : mR.mAvVisSSBO.buffer;
    // This entry's select set (its previous use has completed).
    const VkDescriptorBufferInfo bi[7] = {{mSnapCmd.buffer, 0, VK_WHOLE_SIZE},
                                          {mSnapMeta.buffer, 0, VK_WHOLE_SIZE},
                                          {mR.mSlotSSBO.buffer, 0, VK_WHOLE_SIZE},
                                          {mR.mLodGroupSSBO.buffer, 0, VK_WHOLE_SIZE},
                                          {mR.mAvVisSSBO.buffer, 0, VK_WHOLE_SIZE},
                                          {skin_meta, 0, VK_WHOLE_SIZE},
                                          {mLists.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[7]{};
    for (uint32_t i = 0; i < 7; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mSetSel[entry];
        w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 7, w, 0, nullptr);

    PickSelectPush p{};
    p.w_org[0] = req.origin[0]; p.w_org[1] = req.origin[1]; p.w_org[2] = req.origin[2]; p.w_org[3] = req.length;
    p.w_dir[0] = req.dir[0]; p.w_dir[1] = req.dir[1]; p.w_dir[2] = req.dir[2];
    p.h_org[0] = req.hud_origin[0]; p.h_org[1] = req.hud_origin[1]; p.h_org[2] = req.hud_origin[2];
    p.h_org[3] = req.hud_length;
    p.h_dir[0] = req.hud_dir[0]; p.h_dir[1] = req.hud_dir[1]; p.h_dir[2] = req.hud_dir[2];
    const uint32_t rows = (uint32_t)(mR.mCpuInstances.size() / kInstanceBytes);
    p.job[0] = rows;
    p.job[1] = kListCap;
    p.job[2] = kHudListCap;
    // (An empty window skips its layer: a page that only needs the other one.)
    p.job[3] = (req.win[1] > req.win[0] ? 1u : 0u) | (req.hud && req.win[3] > req.win[2] ? 2u : 0u)
             | ((req.mode & LLVKPickRequest::MODE_ALL_STATIC) ? 4u : 0u);   // <SS:Skoomastream>
    p.valid[0] = mR.mIdxValid;
    p.valid[1] = mR.mGeomVertEnd;
    p.valid[2] = mSnapRows;
    std::memcpy(p.win, req.win, sizeof(p.win));

    // The snapshot copy / earlier indirect reads of the lists (WAR) -> the counter reset
    // and the select.
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                        | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                   | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdFillBuffer(cmd, mLists.buffer, 0, kListHdrBytes, 0u);
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    const VkDescriptorSet sets[2] = {mR.mDsetDraw0, mSetSel[entry]};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSelPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSelLayout, 0, 2, sets, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(p);
    vkCmdPushConstants(cmd, mSelLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (rows + 63u) / 64u, 1, 1);
    // The candidate counts into the entry header (diagnostics + the overflow bits).
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy cp{0, (VkDeviceSize)entry * kEntryBytes + kHdrSel, 16};
    vkCmdCopyBuffer(cmd, mLists.buffer, mOut.buffer, 1, &cp);
    // CLUSTER LOD (#702 integration): the cluster-drawn objects' clusters the ray crosses.
    if (mClPick[entry])
    {
        const float len = req.length > 0.f ? req.length : 1.0e30f;
        mClPick[entry] = mR.mCluster->recordPickFilter(cmd, req.origin, req.dir, req.win[0], std::min(req.win[1], len),
                                                       req.pixel_angle);
    }
    return true;
}

bool LLCleanPickVK::clusterPickOn(const LLVKPickRequest& req)
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    return mR.mCluster && mR.clusterViewDrawWanted(16u) && req.win[1] > req.win[0] && mDslOut
        && mR.mCluster->pickReady(mDslOut);
#else
    (void)req;
    return false;
#endif
}

bool LLCleanPickVK::clusterPickArmed()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    // (= clusterPickOn without the request window: an empty window picks no world geometry)
    return mR.mCluster && mR.clusterViewDrawWanted(16u) && ensureBase() && mDslOut && mR.mCluster->pickReady(mDslOut);
#else
    return false;
#endif
}

void LLCleanPickVK::recordSelect(VkCommandBuffer cmd, const float cam_time[4], float hide_self_w)
{
    mFrame.selected = false;
    if (mR.mOffscreen) return;
    recordSnapshot(cmd, cam_time, hide_self_w);
    if (!mReq.valid) return;
    const uint32_t s = slotIndex();
    SlotState& st = mSlot[s];
    st.pending = false;
    st.req = mReq;
    // (This slot's previous frame completed: its fence was waited, and its answer was
    // harvested in beginFrame.)
    writeHeader(s, mReq);
    mClPick[s] = clusterPickOn(mReq);
    if (!recordSelectInto(cmd, mReq, s)) return;
    mFrame.selected = true;
    mFrame.slot = s;
    std::memcpy(mFrame.cam_time, cam_time, sizeof(mFrame.cam_time));
    mFrame.hide_self_w = hide_self_w;
}

// The layers' draw views. World: the request's projection about its apex, re-based on
// the eye -- the static VS subtracts u_origin, the rigged one's stream is relative to
// cam_time.xyz -- so clip = P * (world - apex) for both. HUD: the request's projection.
void LLCleanPickVK::drawViews(const LLVKPickRequest& rq, const float cam_time[4], float hide_self_w,
                              VKDrawPush& wv, VKDrawPush& hv, bool cluster) const
{
    wv = VKDrawPush{};
    std::memcpy(wv.view_proj, rq.world_vp, sizeof(wv.view_proj));
    for (int r = 0; r < 4; ++r)
    {
        float t = 0.f;
        for (int c = 0; c < 3; ++c) t += rq.world_vp[c * 4 + r] * (cam_time[c] - rq.apex[c]);
        wv.view_proj[12 + r] += t;
    }
    std::memcpy(wv.cam_time, cam_time, sizeof(wv.cam_time));
    wv.origin[0] = cam_time[0]; wv.origin[1] = cam_time[1]; wv.origin[2] = cam_time[2];
    wv.ambient_color[3] = hide_self_w;   // the main view's own-avatar hide (#554)
    // Cluster LOD (#702 integration): the cluster pick draws the switched avatars -- the legacy
    // rigged layer drops them (RC_CLUSTER of the snapshot's main cull), as the main view does.
    wv.sky_zenith[3] = cluster ? 1.f : 0.f;
    hv = VKDrawPush{};
    std::memcpy(hv.view_proj, rq.hud_vp, sizeof(hv.view_proj));
    std::memcpy(hv.cam_time, cam_time, sizeof(hv.cam_time));   // (origin 0: HUD space)
}

void LLCleanPickVK::recordDrawInto(VkCommandBuffer cmd, const LLVKPickRequest& rq, uint32_t entry,
                                   uint32_t view_w, uint32_t view_h)
{
    // The select's lists -> the indirect draws; the target's previous use (WAW).
    {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                         | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ib.srcAccessMask = 0;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        ib.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = mTarget.image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &d);
    }
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = mTarget.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {1u, 1u}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {1u, 1u}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    // (Set 2 is ours; every pass recorded after this one binds its own sets: bindDrawSets.)
    const VkDescriptorSet sets[3] = {mR.mDsetDraw0, mR.mDsetDraw1, mSetOut};
    const VkDeviceSize off = 0;
    const VkDeviceSize rigged_at = kListHdrBytes + (VkDeviceSize)kListCap * kDrawCmdBytes;
    const VkDeviceSize hud_at = kListHdrBytes + (VkDeviceSize)(2u * kListCap) * kDrawCmdBytes;
    // One draw round: the three lists, their counts at count words cw + 0 / 1 / 2.
    auto drawRound = [&](uint32_t cw)
    {
        VKDrawViewPush push{};
        push.pad0 = entry;   // the result entry
        bindGraphicsPipeline(cmd, mStaticPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDrawLayout, 0, 3, sets, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &mR.mVbo.buffer, &off);
        vkCmdBindIndexBuffer(cmd, mR.mIbo.buffer, 0, VK_INDEX_TYPE_UINT16);
        push.view = view_w;
        push.pad1 = rq.mode;
        vkCmdPushConstants(cmd, mDrawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDrawIndexedIndirectCount(cmd, mLists.buffer, kListHdrBytes, mLists.buffer, (cw + 0u) * sizeof(uint32_t),
                                      kListCap, kDrawCmdBytes);
        if (rq.hud)
        {
            push.view = view_h;
            push.pad1 = rq.mode | kPadLayerHud;
            vkCmdPushConstants(cmd, mDrawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexedIndirectCount(cmd, mLists.buffer, hud_at, mLists.buffer, (cw + 2u) * sizeof(uint32_t),
                                          kHudListCap, kDrawCmdBytes);
        }
        if (mR.mSkinVbo.buffer)
        {
            bindGraphicsPipeline(cmd, mRigPipe);
            vkCmdBindVertexBuffers(cmd, 0, 1, &mR.mSkinVbo.buffer, &off);
            push.view = view_w;
            push.pad1 = rq.mode;
            vkCmdPushConstants(cmd, mDrawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDrawIndexedIndirectCount(cmd, mLists.buffer, rigged_at, mLists.buffer, (cw + 1u) * sizeof(uint32_t),
                                          kListCap, kDrawCmdBytes);
        }
        // CLUSTER LOD (#702 integration): the cluster-drawn objects' clusters on the ray, through
        // the cluster vertex-pulling VS and this FS (its output set at 3). Last in the round:
        // the next round binds its own sets.
        if (mClPick[entry] && mR.mCluster)
        {
            const VkDescriptorSet dsets[2] = {mR.mDsetDraw0, mR.mDsetDraw1};
            const uint32_t cpush[4] = {view_w, 0u, entry, rq.mode};
            mR.mCluster->recordPickDraw(cmd, dsets, mSetOut, cpush, cw != 0u ? 1u : 0u);
        }
    };
    drawRound(0u);
    vkCmdEndRendering(cmd);
    if (ensureWindow())
    {
        // COUNT-THEN-FILL (clean_pick_window.comp): round 1's counts / histograms -> the
        // window narrowing -> round 2 (its count words are zero unless a layer overflowed).
        computeBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        // (+ cluster LOD, #702 integration: the cluster pick list, whose round-2 count it arms)
        const uint64_t cla = (mClPick[entry] && mR.mCluster) ? mR.mCluster->pickListAddress() : 0ull;
        const uint32_t wp[4] = {entry, rq.hud ? 1u : 0u, (uint32_t)cla, (uint32_t)(cla >> 32)};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mWinPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mWinLayout, 0, 1, &mSetWin, 0, nullptr);
        vkCmdPushConstants(cmd, mWinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wp), wp);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                        | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                         | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        drawRound(4u);
        vkCmdEndRendering(cmd);
    }
    // Completion marker last: the serial (readEntry ignores an entry without it -- a frame
    // dropped before submit, or recorded without these draws).
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cmd, mOut.buffer, (VkDeviceSize)entry * kEntryBytes + kHdrSerial, 4, rq.serial);
    hostBarrier(cmd);
}

void LLCleanPickVK::recordDraw(VkCommandBuffer cmd)
{
    if (!mFrame.selected) { recordAux(cmd); return; }   // <SS:Skoomastream>
    mFrame.selected = false;
    const uint32_t s = mFrame.slot;
    SlotState& st = mSlot[s];
    const LLVKPickRequest& rq = st.req;
    if (!ensureDraw() || !mR.mDsetDraw0 || !mR.mDsetDraw1 || !mR.mVbo.buffer || !mR.mIbo.buffer) return;
    VKDrawPush wv, hv;
    drawViews(rq, mFrame.cam_time, mFrame.hide_self_w, wv, hv, mClPick[s]);
    const uint32_t view_w = mR.ringAlloc(mR.mViewRgn, &wv, (uint32_t)sizeof(VKDrawPush));
    const uint32_t view_h = rq.hud ? mR.ringAlloc(mR.mViewRgn, &hv, (uint32_t)sizeof(VKDrawPush)) : 0u;
    recordDrawInto(cmd, rq, s, view_w, view_h);
    st.pending = true;
    recordAux(cmd);   // <SS:Skoomastream>
}

bool LLCleanPickVK::pickNow(const LLVKPickRequest& req, LLVKPickResult& out)
{
    out = LLVKPickResult{};
    mNowFail = "";
    if (!req.valid) { mNowFail = "invalid request"; return false; }
    if (!mCtx) { mNowFail = "pick not set up yet (no main frame recorded)"; return false; }
    if (!mSnapRows) { mNowFail = "no main-view snapshot yet (no instance rows / cull scratch)"; return false; }
    if (!ensureSelect()) { mNowFail = "select pipeline unavailable (draw set 0 layout missing)"; return false; }
    if (!ensureDraw()) { mNowFail = "draw pipelines unavailable (draw set layouts missing)"; return false; }
    if (!mR.mDsetDraw0 || !mR.mDsetDraw1 || !mR.mVbo.buffer || !mR.mIbo.buffer)
    {
        mNowFail = "geometry buffers / draw sets missing";
        return false;
    }
    // The sync entry's previous use completed inside its own submission.
    writeHeader(kSyncEntry, req);
    // Cluster LOD (#702 integration): against the last frame's main view (its slot's set; the
    // single arena / counters / skinned arena still hold that frame's data between frames).
    mClPick[kSyncEntry] = clusterPickOn(req);
    // Draw views: the tail of the last recorded frame slot's region (beyond what that frame
    // uses; the next frame of that slot starts after this submission has completed). Never
    // deduplicated (ringReserve): nothing else may alias them.
    VKDrawPush wv, hv;
    drawViews(req, mSnapCam, mSnapHide, wv, hv, mClPick[kSyncEntry]);
    const uint32_t view_w = mR.ringReserve(mR.mViewRgn, &wv, (uint32_t)sizeof(VKDrawPush));
    const uint32_t view_h = req.hud ? mR.ringReserve(mR.mViewRgn, &hv, (uint32_t)sizeof(VKDrawPush)) : 0u;
    bool recorded = false;
    mCtx->submitImmediate([&](VkCommandBuffer cmd)
    {
        // Every earlier submission (the frames in flight: their cull / pre-skin / snapshot
        // writes, their indirect reads of the lists) -> this pass.
        computeBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        if (!recordSelectInto(cmd, req, kSyncEntry)) return;
        recordDrawInto(cmd, req, kSyncEntry, view_w, view_h);
        recorded = true;
    });
    if (!recorded)
    {
        mNowFail = "select could not record (snapshot / slot / LOD / avatar tables missing)";
        return false;
    }
    readEntry(kSyncEntry, req, out);
    if (!out.valid) mNowFail = "GPU answer missing (submission did not complete)";
    return out.valid;
}

// ---- Drawn-geometry queries (#704; clean_pick_query.comp) ----

namespace
{
// clean_pick_query.comp PickQueryPush (96 B).
struct PickQueryPush
{
    float    cam[4];    // eye, tan_x
    float    at[4];     // view axis, tan_y
    float    left[4];   // left axis, viewport x
    float    up[4];     // up axis, viewport y
    float    rect[4];   // left, right, bottom, top
    uint32_t job[4];    // jobs, snapshot rows, mode (bit 0 rect), viewport w | h << 16
};
static_assert(sizeof(PickQueryPush) == 96 && sizeof(PickQueryPush) <= kVkPushConstantBudget, "PickQueryPush");
constexpr uint32_t kQueryOutBytes = 64;   // QueryOut
constexpr uint32_t kQueryResolved = 1u, kQueryIn = 2u, kQueryOut = 4u;
} // namespace

bool LLCleanPickVK::ensureQuery(uint32_t jobs)
{
    if (mQueryFailed || !ensureBase()) return false;
    VkDevice dev = mCtx->device();
    if (!mDslQuery)
    {
        // 0 snapshot cmd, 1 snapshot meta, 2 slots, 3 LOD groups, 4 jobs, 5 results.
        VkDescriptorSetLayoutBinding b[6]{};
        for (uint32_t i = 0; i < 6; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        l.bindingCount = 6; l.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &l, &mDslQuery, "mDslPickQuery"));
        VK_CHECK(mQueryPool.create(dev, "pickQuery", {{mDslQuery, 1}}));
        VKD_ALLOC_SOFT(mQueryPool.allocate(mDslQuery, &mSetQuery), { mQueryFailed = true; return false; });
        if (!mSetQuery)
        {
            mQueryFailed = true;
            return false;
        }
    }
    if (jobs > mQueryCap)
    {
        // (Between frames, after the wait: nothing reads the old pair.)
        VkMemScope memcat(VKMEM_CULL);   // #407 (cursor pick)
        const uint32_t cap = std::max<uint32_t>(jobs + jobs / 2u, 256u);
        GpuBuffer in = mCtx->createBuffer((VkDeviceSize)cap * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        GpuBuffer out = mCtx->createBuffer((VkDeviceSize)cap * kQueryOutBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!in.buffer || !in.mapped || !out.buffer || !out.mapped)
        {
            // #698: no crash -- this query answers nothing; a later one retries.
            if (in.buffer) mCtx->destroyBuffer(in);
            if (out.buffer) mCtx->destroyBuffer(out);
            LL_WARNS("Vulkan") << "drawn-geometry query: buffers for " << jobs << " jobs failed" << LL_ENDL;
            return false;
        }
        if (mQueryIn.buffer) mCtx->destroyBuffer(mQueryIn);
        if (mQueryOut.buffer) mCtx->destroyBuffer(mQueryOut);
        mQueryIn = in;
        mQueryOut = out;
        mQueryCap = cap;
    }
    const VkDescriptorSetLayout want = mR.mDslDraw0;
    if (mQueryPipe && mQueryLayoutDsl == want) return true;
    if (!want) return false;
    if (mQueryPipe || mQueryLayout)
    {
        if (mQueryPipe) vkDestroyPipeline(dev, mQueryPipe, nullptr);
        if (mQueryLayout) vkDestroyPipelineLayout(dev, mQueryLayout, nullptr);
        mQueryPipe = VK_NULL_HANDLE; mQueryLayout = VK_NULL_HANDLE;
    }
    const VkDescriptorSetLayout sets[2] = {want, mDslQuery};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PickQueryPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2; li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mQueryLayout, "mPickQueryLayout") != VK_SUCCESS)
    {
        mQueryLayout = VK_NULL_HANDLE;
        mQueryFailed = true;
        return false;
    }
    VkShaderModule m = pickModule(dev, kWorldPickQuerySpv, kWorldPickQuerySpv_bytes);
    mQueryPipe = m ? createComputePipeline(dev, m, mQueryLayout) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mQueryPipe)
    {
        LL_WARNS("Vulkan") << "drawn-geometry query: pipeline creation failed" << LL_ENDL;
        mQueryFailed = true;
        return false;
    }
    mQueryLayoutDsl = want;
    return true;
}

bool LLCleanPickVK::queryNow(const std::vector<uint32_t>& ids, const LLVKRectQuery* rect,
                             std::vector<LLVKFaceQueryResult>& out)
{
    out.assign(ids.size(), LLVKFaceQueryResult{});
    if (ids.empty()) return true;
    if (!mCtx || !mSnapRows || !mSnapCmd.buffer || !mSnapMeta.buffer || !mR.mDsetDraw0 || !mR.mSlotSSBO.buffer
        || !mR.mLodGroupSSBO.buffer || !ensureQuery((uint32_t)ids.size()))
        return false;
    // Between frames: the previous query's submission has completed.
    std::memcpy(mQueryIn.mapped, ids.data(), ids.size() * sizeof(uint32_t));
    std::memset(mQueryOut.mapped, 0, ids.size() * kQueryOutBytes);
    const VkDescriptorBufferInfo bi[6] = {{mSnapCmd.buffer, 0, VK_WHOLE_SIZE},
                                          {mSnapMeta.buffer, 0, VK_WHOLE_SIZE},
                                          {mR.mSlotSSBO.buffer, 0, VK_WHOLE_SIZE},
                                          {mR.mLodGroupSSBO.buffer, 0, VK_WHOLE_SIZE},
                                          {mQueryIn.buffer, 0, VK_WHOLE_SIZE},
                                          {mQueryOut.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mSetQuery;
        w[i].dstBinding = i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mCtx->device(), 6, w, 0, nullptr);
    PickQueryPush p{};
    if (rect)
    {
        std::memcpy(p.cam, rect->eye, 3 * sizeof(float));   p.cam[3] = rect->tan_x;
        std::memcpy(p.at, rect->at, 3 * sizeof(float));     p.at[3] = rect->tan_y;
        std::memcpy(p.left, rect->left, 3 * sizeof(float)); p.left[3] = rect->viewport[0];
        std::memcpy(p.up, rect->up, 3 * sizeof(float));     p.up[3] = rect->viewport[1];
        std::memcpy(p.rect, rect->rect, sizeof(p.rect));
        p.job[2] = 1u;
        p.job[3] = (std::min<uint32_t>((uint32_t)std::max(rect->viewport[2], 0.f), 0xFFFFu))
                 | (std::min<uint32_t>((uint32_t)std::max(rect->viewport[3], 0.f), 0xFFFFu) << 16u);
    }
    p.job[0] = (uint32_t)ids.size();
    p.job[1] = mSnapRows;
    mCtx->submitImmediate([&](VkCommandBuffer cmd)
    {
        // Every earlier submission (the snapshot, the instance / slot uploads) -> the query.
        computeBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        const VkDescriptorSet sets[2] = {mR.mDsetDraw0, mSetQuery};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mQueryPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mQueryLayout, 0, 2, sets, 0, nullptr);
        VK_PUSH_BUDGET_CHECK(p);
        vkCmdPushConstants(cmd, mQueryLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, (uint32_t)ids.size(), 1, 1);
        hostBarrier(cmd);
    });
    const uint8_t* o = reinterpret_cast<const uint8_t*>(mQueryOut.mapped);
    for (size_t i = 0; i < ids.size(); ++i, o += kQueryOutBytes)
    {
        float nsum[4], n0[4], t0[4];
        uint32_t res[4];
        std::memcpy(nsum, o, 16);
        std::memcpy(n0, o + 16, 16);
        std::memcpy(t0, o + 32, 16);
        std::memcpy(res, o + 48, 16);
        LLVKFaceQueryResult& r = out[i];
        r.valid = (res[0] & kQueryResolved) != 0u;
        if (!r.valid) continue;
        std::memcpy(r.normal_sum, nsum, 3 * sizeof(float));
        r.verts = res[2];
        std::memcpy(r.normal0, n0, 3 * sizeof(float));
        std::memcpy(r.tangent0, t0, 4 * sizeof(float));
        r.any_in = (res[0] & kQueryIn) != 0u;
        r.any_out = (res[0] & kQueryOut) != 0u;
    }
    return true;
}

// ---- Particle pick (#481) ----

// The particle pool exists and draws this frame (the renderer's own gates; the CPU side
// checks the particle render type).
bool LLCleanPickVK::partReady() const
{
    return mR.mPartOn && mR.mPartCap > 0 && mR.mPartMax > 0 && mR.mPartPool.buffer
        && mR.mDsetPartC && mR.mDslPartC && mR.mDsetDraw1 && mR.mDslDraw1;
}

// Result ring + per-entry partial buffers + the set layout and sets, and the pipeline
// (rebuilt if a set layout it was made with changed).
bool LLCleanPickVK::ensurePart()
{
    VkMemScope memcat(VKMEM_CULL);   // #407 (cursor pick)
    if (mPartFailed || !mR.mCtx) return false;
    mCtx = mR.mCtx;
    VkDevice dev = mCtx->device();
    if (!mPartOut.buffer)
    {
        mPartOut = mCtx->createBuffer((VkDeviceSize)(kSlots + 1) * kPartResultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!mPartOut.buffer || !mPartOut.mapped)
        {
            LL_WARNS("Vulkan") << "particle pick: result buffer allocation failed; particle picking off" << LL_ENDL;
            mPartFailed = true;
            return false;
        }
        std::memset(mPartOut.mapped, 0, (size_t)mPartOut.size);   // serial 0 = never written
    }
    if (!mDslPart)
    {
        // 0 partial (t bits, index) per pool workgroup, 1 result ring.
        VkDescriptorSetLayoutBinding b[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo l{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        l.bindingCount = 2; l.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &l, &mDslPart, "mDslPickPart"));
        VK_CHECK(mPartDescPool.create(dev, "pickPart", {{mDslPart, kSlots + 1}}));
        for (uint32_t i = 0; i <= kSlots; ++i) VKD_ALLOC_SOFT(mPartDescPool.allocate(mDslPart, &mSetPart[i]), { mPartFailed = true; return false; });
        if (!mDslPart || !mSetPart[kSlots])
        {
            mPartFailed = true;
            return false;
        }
    }
    const uint32_t groups = (mR.mPartMax + 255u) / 256u;
    if (groups > mPartGroups)
    {
        if (mPartGroups) vkDeviceWaitIdle(dev);   // the pool grew (rare): reallocate
        for (GpuBuffer& b : mPartPartial)
        {
            if (b.buffer) mCtx->destroyBuffer(b);
            b = mCtx->createBuffer((VkDeviceSize)groups * 2u * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (!b.buffer)
            {
                LL_WARNS("Vulkan") << "particle pick: partial buffers failed; particle picking off" << LL_ENDL;
                mPartFailed = true;
                return false;
            }
        }
        mPartGroups = groups;
        for (uint32_t i = 0; i <= kSlots; ++i)
        {
            VkDescriptorBufferInfo bi[2] = {{mPartPartial[i].buffer, 0, VK_WHOLE_SIZE},
                                            {mPartOut.buffer, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[2]{};
            for (uint32_t k = 0; k < 2; ++k)
            {
                w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = mSetPart[i];
                w[k].dstBinding = k; w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi[k];
            }
            vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
        }
    }
    const VkDescriptorSetLayout want[2] = {mR.mDslPartC, mR.mDslDraw1};
    if (mPartPipe && std::equal(want, want + 2, mPartLayoutDsl)) return true;
    if (!want[0] || !want[1]) return false;
    if (mPartPipe || mPartLayout)
    {
        vkDeviceWaitIdle(dev);   // a set layout changed under it (rare): rebuild
        if (mPartPipe) vkDestroyPipeline(dev, mPartPipe, nullptr);
        if (mPartLayout) vkDestroyPipelineLayout(dev, mPartLayout, nullptr);
        mPartPipe = VK_NULL_HANDLE; mPartLayout = VK_NULL_HANDLE;
    }
    // Set 0 = the particle compute set (0 pool, 1 emitters), 1 = draw set 1 (bindless
    // textures), 2 = ours.
    const VkDescriptorSetLayout sets[3] = {want[0], want[1], mDslPart};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PartPickPush)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 3; li.pSetLayouts = sets;
    li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mPartLayout, "mPickPartLayout") != VK_SUCCESS)
    {
        mPartLayout = VK_NULL_HANDLE;
        mPartFailed = true;
        return false;
    }
    VkShaderModule m = pickModule(dev, kWorldPickParticleSpv, kWorldPickParticleSpv_bytes);
    mPartPipe = m ? createComputePipeline(dev, m, mPartLayout) : VK_NULL_HANDLE;
    if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (!mPartPipe)
    {
        LL_WARNS("Vulkan") << "particle pick: pipeline creation failed" << LL_ENDL;
        mPartFailed = true;
        return false;
    }
    std::copy(want, want + 2, mPartLayoutDsl);
    return true;
}

void LLCleanPickVK::recordPartDispatch(VkCommandBuffer cmd, const LLVKPickRequest& req, uint32_t entry)
{
    PartPickPush p{};
    std::memcpy(p.origin, req.origin, sizeof(req.origin));
    p.origin[3] = req.length;
    std::memcpy(p.dir, req.dir, sizeof(req.dir));
    p.dir[3] = req.alpha_threshold;
    // The main view's world -> view (column-major): its rotation rows are the billboard
    // basis the particle VS builds on (recordParticleDraw: the u_inv_view_proj slot).
    const float* V = mR.mLightInfoCPU.view;
    p.right[0] = V[0];  p.right[1] = V[4];  p.right[2] = V[8];
    p.up[0]    = V[1];  p.up[1]    = V[5];  p.up[2]    = V[9];
    p.fwd[0]   = -V[2]; p.fwd[1]   = -V[6]; p.fwd[2]   = -V[10];
    p.hdr[0] = req.serial; p.hdr[1] = entry; p.hdr[2] = 0u; p.hdr[3] = mR.mPartMax;
    const uint32_t groups = (mR.mPartMax + 255u) / 256u;

    const VkDescriptorSet sets[3] = {mR.mDsetPartC, mR.mDsetDraw1, mSetPart[entry]};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPartPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPartLayout, 0, 3, sets, 0, nullptr);
    VK_PUSH_BUDGET_CHECK(p);
    vkCmdPushConstants(cmd, mPartLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, groups, 1, 1);
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    p.hdr[2] = 1u;
    vkCmdPushConstants(cmd, mPartLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, 1, 1, 1);
    hostBarrier(cmd);
    // Our pool reads before any later compute rewrites it (the next frame's particle sim).
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0);
}

void LLCleanPickVK::recordParticles(VkCommandBuffer cmd)
{
    if (!mPartReq.valid) return;
    const uint32_t s = slotIndex();
    PartSlotState& st = mPartSlot[s];
    st.pending = false;
    const LLVKPickRequest req = mPartReq;
    mPartReq.valid = false;   // one dispatch per request
    if (!partReady() || !ensurePart()) return;
    st.req = req;
    // This frame's particle compute (sim / emit / sort writes) -> our pool reads; the
    // previous use of this entry's partial / result (WAR) is in the first scope too.
    computeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    recordPartDispatch(cmd, st.req, s);
    st.pending = true;
}

void LLCleanPickVK::readPartEntry(uint32_t entry, const LLVKPickRequest& req, LLVKPartPickResult& out) const
{
    out = LLVKPartPickResult{};
    if (!mPartOut.mapped) return;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(mPartOut.mapped) + (size_t)entry * kPartResultBytes;
    uint32_t hdr[4], ids[4];
    float pos_t[4];
    std::memcpy(hdr, base, 16);
    std::memcpy(ids, base + 16, 16);
    std::memcpy(pos_t, base + 32, 16);
    if (hdr[0] != req.serial) return;   // not this request's answer (never written / stale)
    out.valid = true;
    out.serial = hdr[0];
    std::memcpy(out.origin, req.origin, sizeof(out.origin));
    std::memcpy(out.dir, req.dir, sizeof(out.dir));
    out.length = req.length;
    out.hit = hdr[1] != 0u && hdr[2] != 0xFFFFFFFFu;
    if (!out.hit) return;
    out.index = hdr[2];
    out.emitter = hdr[3];
    out.source_id = ids[0];
    out.t = pos_t[3];
    std::memcpy(out.pos, pos_t, 3 * sizeof(float));
}

bool LLCleanPickVK::latestPart(LLVKPartPickResult& out) const
{
    if (!mPartLatest.valid) return false;
    out = mPartLatest;
    return true;
}

bool LLCleanPickVK::partNow(const LLVKPickRequest& req, LLVKPartPickResult& out)
{
    out = LLVKPartPickResult{};
    if (!req.valid || !partReady() || !ensurePart()) return false;
    // Between frames: the pool the last frame drew is complete once the frames in flight
    // are, and nothing rewrites it before the next recording. The first barrier makes
    // their writes visible (and orders the sync entry's previous use).
    mCtx->submitImmediate([&](VkCommandBuffer cmd)
    {
        computeBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        recordPartDispatch(cmd, req, kSyncEntry);
    });
    readPartEntry(kSyncEntry, req, out);
    return out.valid;
}
