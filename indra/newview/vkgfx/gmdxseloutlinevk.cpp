/**
 * @file gmdxseloutlinevk.cpp
 * @brief GunMetal-DX selection styles for the Vulkan clean renderer (GMDXSelectionStyle).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> How the edit selection is drawn (GMDXSelectionStyle; the CPU half is
// newview/gmdxselectionoutline.cpp). Defines the LLCleanRendererVK members declared in its
// "SELECTION STYLE" block, so the renderer file only carries the call sites.
//
// 0 CLASSIC. Prims and sculpties get LL's CPU silhouette (drawn through gGL, not here); what
//   LL has no silhouette for -- mesh, rigged mesh, avatars, trees -- gets Firestorm's mesh
//   wireframe (LLSelectMgr::renderSilhouettes' renderMeshSelection_f / LLFace::renderOneWireframe):
//   visible edges in the selection colour x 2 at 40 % opacity, LL's 5 px lines where the
//   device has wideLines (else 1 px) pushed back by LL's polygon offset (3, 3) so only the
//   outline and creases clear the surface, and with hidden selections on LL's hidden pass:
//   depth GEQUAL, blended (SRC_COLOR, ONE). Colours: Firestorm's (GMDXClassic* in
//   colors.xml, via GMDXSelectionOutline::parentColor / childColor). Same lists and passes
//   as Slipstream's wireframe, own pipelines (built on first use).
//
// 1 OUTLINE. Everything, on the GPU in screen space, no CPU silhouette:
//   a) the selection lists (static + rigged world, or HUD) drawn FILLED into an R16_UINT mask
//      at display size against the scene depth: hidden pass (GREATER) first, then the visible
//      pass over it -- kind (root / child / hover), occluded bit, a 13-bit face hash
//      (gmdx_sel_mask.frag) -- and each touched 16 x 16 tile marked in a small buffer;
//   b) one edge pass drawn as a quad per marked tile (and its neighbours), so it never covers
//      the whole screen: outer contour + face edges (gmdx_sel_outline.frag), blended over the
//      finished frame where Slipstream's selection lines go (after post / tonemap; HUD: after
//      the HUD buckets).
//   Nothing runs while nothing is selected; the mask (2 B / px) and tile buffer are made on
//   first use and remade on a resize.
//
// 2 WIREFRAME. Slipstream's every-triangle wireframe for everything (recordSelection's
//   original pipelines).

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llviewercontrol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

struct GMDXSelOutlineVK
{
    // Classic: [pair * 2 + pass], pair 0 world static, 1 HUD, 2 world rigged; pass 0 hidden, 1 visible.
    VkPipeline classic[6] = {};
    VkFormat   classicFormat = VK_FORMAT_UNDEFINED;
    bool       classicFailed = false;

    // Outline: mask pipelines in the same order, the edge pass, layouts, sets.
    VkPipeline mask[6] = {};
    VkPipeline composite = VK_NULL_HANDLE;
    VkFormat   compositeFormat = VK_FORMAT_UNDEFINED;
    bool       outlineFailed = false;
    VkDescriptorSetLayout maskDsl = VK_NULL_HANDLE, compDsl = VK_NULL_HANDLE;
    VkPipelineLayout      maskLayout = VK_NULL_HANDLE, compLayout = VK_NULL_HANDLE;
    VkSampler             sampler = VK_NULL_HANDLE;
    VkDescPool            pool;
    VkDescriptorSet       maskSet[2] = {}, compSet[2] = {};
    uint64_t              setGen[2] = {0, 0};

    // Targets (display size).
    GpuImage  maskImg{};
    GpuBuffer tiles{};
    uint32_t  w = 0, h = 0, tilesX = 0, tilesY = 0;
    uint64_t  gen = 1;
};

namespace
{
constexpr VkFormat kMaskFormat = VK_FORMAT_R16_UINT;
constexpr VkDeviceSize kDrawCmdBytes = sizeof(VkDrawIndexedIndirectCommand);   // DrawCmd (20 B)
constexpr float kOccludedAlpha = 0.35f;   // Slipstream's dimming of occluded selection edges

// gmdx_sel_outline_common.glsl SelOutlinePush.
struct SelOutlinePush { float colors[3][4]; float dims[4]; float params[4]; };
static_assert(sizeof(SelOutlinePush) == 80, "SelOutlinePush must match gmdx_sel_outline_common.glsl");

VkShaderModule selModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

// The lit pass's vertex input / depth format / reversed-Z with every attachment and blend
// extra stripped: the base of the selection pipelines (as llcelshadevk.cpp's id pass).
GraphicsPipelineDesc selBaseDesc(const GraphicsPipelineDesc& lit)
{
    GraphicsPipelineDesc d = lit;
    d.task = d.mesh = VK_NULL_HANDLE;
    d.glowFormat = VK_FORMAT_UNDEFINED; d.glowWrite = false;
    d.reactFormat = VK_FORMAT_UNDEFINED; d.reactWriteMask = 0;
    d.mrtCount = 0;
    d.colorWrite = true; d.noColor = false;
    d.blend = false; d.additive = false; d.customBlend = false; d.partAccum = false; d.glowAdditive = false;
    d.depthTest = true; d.depthWrite = false; d.depthClamp = false;
    d.depthBiasConstant = 0.f; d.depthBiasSlope = 0.f;
    d.cullMode = VK_CULL_MODE_NONE; d.polygonMode = VK_POLYGON_MODE_FILL; d.lineWidth = 1.f;
    d.fragSpec = nullptr; d.fsrAttachment = false; d.dynamicShadingRate = false;
    return d;
}

// Visible: LEQUAL pulled toward the camera (as mSelVisPipe); hidden: GREATER (outline mask) or
// LL's GEQUAL (classic). HUD pipelines test the HUD pass's conventional depth.
void setPassDepth(GraphicsPipelineDesc& d, int pass, bool hud, bool lit_reverse_z, VkCompareOp hidden_op)
{
    d.reverseZ = hud ? false : lit_reverse_z;
    if (pass == 1)
    {
        d.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
        d.depthBiasConstant = -1.f; d.depthBiasSlope = -1.f;
    }
    else
    {
        d.depthCompare = hidden_op;
        d.depthBiasConstant = 0.f; d.depthBiasSlope = 0.f;
    }
}

void barrier(VkCommandBuffer cmd, uint32_t nmem, const VkMemoryBarrier2* mem, const VkImageMemoryBarrier2* img)
{
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = nmem; d.pMemoryBarriers = mem;
    d.imageMemoryBarrierCount = img ? 1u : 0u; d.pImageMemoryBarriers = img;
    vkCmdPipelineBarrier2(cmd, &d);
}

VkMemoryBarrier2 memBarrier(VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = ss; m.srcAccessMask = sa; m.dstStageMask = ds; m.dstAccessMask = da;
    return m;
}

VkImageMemoryBarrier2 imgBarrier(VkImage img, VkImageLayout from, VkImageLayout to,
                                 VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
} // namespace

int LLCleanRendererVK::selectionStyle() const
{
    static LLCachedControl<S32> s_style(gSavedSettings, "GMDXSelectionStyle", 0);
    return llclamp((S32)s_style, 0, 2);
}

void LLCleanRendererVK::selPassColor(uint32_t kind, int pass, bool classic, float out[4]) const
{
    const float* c = selColor(kind);
    if (!classic)
    {
        std::memcpy(out, c, 4 * sizeof(float));
        if (pass == 0) out[3] *= kOccludedAlpha;   // occluded edges dimmed
        return;
    }
    // Firestorm: visible edges diffuseColor4f(colour x 2, sHighlightAlpha = 0.4); the hidden
    // pass draws the colour itself with alpha 0.4 under (SRC_COLOR, ONE) -- the alpha is unused.
    for (int i = 0; i < 3; ++i) out[i] = pass == 1 ? std::min(1.f, c[i] * 2.f) : c[i];
    out[3] = 0.4f;
}

const VkPipeline* LLCleanRendererVK::selClassicPipes()
{
    if (!mCtx) return nullptr;
    if (!mSelOutline) mSelOutline = new GMDXSelOutlineVK();
    GMDXSelOutlineVK& s = *mSelOutline;
    VkDevice dev = mCtx->device();
    if (s.classic[0] && s.classicFormat != mColorFormat)
    {
        vkDeviceWaitIdle(dev);
        for (VkPipeline& p : s.classic) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        s.classicFailed = false;
    }
    if (s.classic[0]) return s.classic;
    if (s.classicFailed) return nullptr;

    VkShaderModule vs  = selModule(dev, kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
    VkShaderModule rvs = selModule(dev, kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes, "kWorldRiggedVertSpv");
    VkShaderModule fs  = selModule(dev, kWorldSelectFragSpv, kWorldSelectFragSpv_bytes, "kWorldSelectFragSpv");
    if (vs && rvs && fs)
    {
        GraphicsPipelineDesc d = selBaseDesc(mLitDesc);
        d.layout = mDrawLayout;
        d.colorFormat = mColorFormat;
        d.frag = fs;
        d.polygonMode = VK_POLYGON_MODE_LINE;
        // LL: setLineWidth(5) (clamped to the device's range); 1 px without wideLines.
        d.lineWidth = mCtx->wideLinesEnabled() ? std::max(1.f, std::min(5.f, mCtx->lineWidthMax())) : 1.f;
        for (int k = 0; k < 6; ++k)
        {
            const int pair = k / 2, pass = k & 1;
            d.vert = pair == 2 ? rvs : vs;
            if (pair == 2) vkWorldRiggedVI(d); else vkWorldStaticVI(d);
            setPassDepth(d, pass, pair == 1, mLitDesc.reverseZ, VK_COMPARE_OP_GREATER_OR_EQUAL);
            // Visible pass: LL's glPolygonOffset(3, 3) -- AWAY from the camera, not toward it
            // as the wireframe's -1. The edges inside a surface then sink behind it and only
            // the outline and the creases show (no triangle-edge "ladder"); the hidden pass
            // has no offset, as in LL.
            if (pass == 1) { d.depthBiasConstant = 3.f; d.depthBiasSlope = 3.f; }
            d.blend = pass == 1;
            d.customBlend = pass == 0;
            d.srcColor = VK_BLEND_FACTOR_SRC_COLOR; d.dstColor = VK_BLEND_FACTOR_ONE;
            s.classic[k] = createGraphicsPipeline(dev, d);
        }
    }
    for (VkShaderModule m : {vs, rvs, fs}) if (m) vkDestroyShaderModule(dev, m, nullptr);
    s.classicFormat = mColorFormat;
    bool ok = true;
    for (VkPipeline p : s.classic) ok = ok && p;
    if (!ok)
    {
        for (VkPipeline& p : s.classic) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        s.classicFailed = true;
        LL_WARNS("GMDXSelection") << "classic selection pipelines failed to build; using the wireframe" << LL_ENDL;
        return nullptr;
    }
    return s.classic;
}

bool LLCleanRendererVK::selOutlineWanted(bool hud) const
{
    if (selectionStyle() != 1) return false;
    uint32_t n = 0;
    for (uint32_t k = 0; k < SEL_KINDS; ++k)
        n += hud ? mSelCount[SEL_KINDS + k] : mSelCount[k] + mSelCount[2 * SEL_KINDS + k];
    return n > 0;
}

// Layouts, sets and pipelines of the outline, built on first use. false = unavailable (logged once).
static bool ensureSelOutline(GMDXSelOutlineVK& s, VkDevice dev, const GraphicsPipelineDesc& lit,
                             VkDescriptorSetLayout draw0, VkDescriptorSetLayout draw1, VkFormat colorFormat)
{
    if (s.outlineFailed) return false;
    if (s.composite && s.compositeFormat != colorFormat)
    {
        vkDeviceWaitIdle(dev);
        vkDestroyPipeline(dev, s.composite, nullptr);
        s.composite = VK_NULL_HANDLE;
    }
    if (s.mask[0] && s.composite) return true;
    auto fail = [&](const char* what)
    {
        LL_WARNS("GMDXSelection") << "selection outline unavailable (" << what << "); nothing outlined in Outline style" << LL_ENDL;
        s.outlineFailed = true;
        return false;
    };
    if (!s.maskDsl)
    {
        VkDescriptorSetLayoutBinding mb{};
        mb.binding = 0; mb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        mb.descriptorCount = 1; mb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ml{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ml.bindingCount = 1; ml.pBindings = &mb;
        if (vkdCreateSetLayout(dev, &ml, &s.maskDsl, "mDslGmdxSelMask") != VK_SUCCESS) s.maskDsl = VK_NULL_HANDLE;
        VkDescriptorSetLayoutBinding cb[2]{};
        cb[0].binding = 0; cb[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        cb[0].descriptorCount = 1; cb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        cb[1].binding = 1; cb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cb[1].descriptorCount = 1; cb[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo cl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        cl.bindingCount = 2; cl.pBindings = cb;
        if (vkdCreateSetLayout(dev, &cl, &s.compDsl, "mDslGmdxSelOutline") != VK_SUCCESS) s.compDsl = VK_NULL_HANDLE;
        if (!s.maskDsl || !s.compDsl
            || s.pool.create(dev, "gmdx_sel_outline", {{s.maskDsl, 2}, {s.compDsl, 2}}) != VK_SUCCESS
            || s.pool.allocate(s.maskDsl, &s.maskSet[0]) != VK_SUCCESS
            || s.pool.allocate(s.maskDsl, &s.maskSet[1]) != VK_SUCCESS
            || s.pool.allocate(s.compDsl, &s.compSet[0]) != VK_SUCCESS
            || s.pool.allocate(s.compDsl, &s.compSet[1]) != VK_SUCCESS)
            return fail("descriptor sets");
        // Mask: draw sets 0 / 1 + the tile buffer at set 2, the draw push range (compatible with
        // mDrawLayout for sets 0 / 1 and the push constants).
        const VkDescriptorSetLayout ms[3] = {draw0, draw1, s.maskDsl};
        VkPushConstantRange mpr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VKDrawViewPush)};
        VkPipelineLayoutCreateInfo mli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        mli.setLayoutCount = 3; mli.pSetLayouts = ms;
        mli.pushConstantRangeCount = 1; mli.pPushConstantRanges = &mpr;
        if (vkdCreatePipelineLayout(dev, &mli, &s.maskLayout, "mGmdxSelMaskLayout") != VK_SUCCESS) s.maskLayout = VK_NULL_HANDLE;
        VkPushConstantRange cpr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SelOutlinePush)};
        VkPipelineLayoutCreateInfo cli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        cli.setLayoutCount = 1; cli.pSetLayouts = &s.compDsl;
        cli.pushConstantRangeCount = 1; cli.pPushConstantRanges = &cpr;
        if (vkdCreatePipelineLayout(dev, &cli, &s.compLayout, "mGmdxSelOutlineLayout") != VK_SUCCESS) s.compLayout = VK_NULL_HANDLE;
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &sci, nullptr, &s.sampler) != VK_SUCCESS) s.sampler = VK_NULL_HANDLE;
        if (!s.maskLayout || !s.compLayout || !s.sampler) return fail("layouts");
    }
    if (!s.mask[0])
    {
        VkShaderModule vs  = selModule(dev, kWorldVertSpv, kWorldVertSpv_bytes, "kWorldVertSpv");
        VkShaderModule rvs = selModule(dev, kWorldRiggedVertSpv, kWorldRiggedVertSpv_bytes, "kWorldRiggedVertSpv");
        VkShaderModule fs  = selModule(dev, kGmdxSelMaskFragSpv, kGmdxSelMaskFragSpv_bytes, "kGmdxSelMaskFragSpv");
        if (vs && rvs && fs)
        {
            GraphicsPipelineDesc d = selBaseDesc(lit);
            d.layout = s.maskLayout;
            d.colorFormat = kMaskFormat;
            d.frag = fs;
            for (int k = 0; k < 6; ++k)
            {
                const int pair = k / 2, pass = k & 1;
                d.vert = pair == 2 ? rvs : vs;
                if (pair == 2) vkWorldRiggedVI(d); else vkWorldStaticVI(d);
                setPassDepth(d, pass, pair == 1, lit.reverseZ, VK_COMPARE_OP_GREATER);
                s.mask[k] = createGraphicsPipeline(dev, d);
            }
        }
        for (VkShaderModule m : {vs, rvs, fs}) if (m) vkDestroyShaderModule(dev, m, nullptr);
        for (VkPipeline p : s.mask) if (!p) return fail("mask pipelines");
    }
    if (!s.composite)
    {
        VkShaderModule vs = selModule(dev, kGmdxSelOutlineVertSpv, kGmdxSelOutlineVertSpv_bytes, "kGmdxSelOutlineVertSpv");
        VkShaderModule fs = selModule(dev, kGmdxSelOutlineFragSpv, kGmdxSelOutlineFragSpv_bytes, "kGmdxSelOutlineFragSpv");
        if (vs && fs)
        {
            GraphicsPipelineDesc d;
            d.vert = vs; d.frag = fs; d.layout = s.compLayout;
            d.colorFormat = colorFormat;
            d.depthFormat = VK_FORMAT_UNDEFINED; d.depthTest = false; d.depthWrite = false;
            d.blend = true; d.cullMode = VK_CULL_MODE_NONE;
            s.composite = createGraphicsPipeline(dev, d);
        }
        for (VkShaderModule m : {vs, fs}) if (m) vkDestroyShaderModule(dev, m, nullptr);
        if (!s.composite) return fail("edge pipeline");
        s.compositeFormat = colorFormat;
        LL_INFOS("GMDXSelection") << "selection outline pipelines built" << LL_ENDL;
    }
    return true;
}

void LLCleanRendererVK::recordSelOutline(VkCommandBuffer cmd, const VkDescriptorSet* dsets, const VKDrawPush& push, bool hud,
                                         VkImageView colorView, const GpuImage& depth, uint32_t width, uint32_t height,
                                         const VkViewport& vp, const VkRect2D& sc)
{
    if (!mCtx || !selOutlineWanted(hud) || colorView == VK_NULL_HANDLE || !depth.view || width == 0 || height == 0)
        return;
    if (!mSelOutline) mSelOutline = new GMDXSelOutlineVK();
    GMDXSelOutlineVK& s = *mSelOutline;
    VkDevice dev = mCtx->device();
    if (!ensureSelOutline(s, dev, mLitDesc, mDslDraw0, mDslDraw1, mColorFormat)) return;

    // ---- targets: display size, remade on a resize ----
    if (!s.maskImg.image || !s.tiles.buffer || s.w != width || s.h != height)
    {
        vkDeviceWaitIdle(dev);   // frames in flight may still read the old ones
        if (s.maskImg.image) mCtx->destroyImage(s.maskImg);
        if (s.tiles.buffer) mCtx->destroyBuffer(s.tiles);
        s.maskImg = GpuImage{}; s.tiles = GpuBuffer{};
        s.w = s.h = 0;
        VkMemScope mem(VKMEM_TARGET);
        s.maskImg = mCtx->createImage2D(width, height, kMaskFormat,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT);
        s.tilesX = (width + 15u) / 16u;
        s.tilesY = (height + 15u) / 16u;
        s.tiles = mCtx->createBuffer((VkDeviceSize)s.tilesX * s.tilesY * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!s.maskImg.image || !s.tiles.buffer) return;
        s.w = width; s.h = height;
        ++s.gen;
    }

    // ---- this frame slot's sets (its previous use has retired; written once per target set) ----
    const uint32_t slot = mFrameSlot & 1u;
    if (s.setGen[slot] != s.gen)
    {
        const VkDescriptorBufferInfo tb{s.tiles.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo mi{s.sampler, s.maskImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[3]{};
        for (VkWriteDescriptorSet& x : w) { x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x.descriptorCount = 1; }
        w[0].dstSet = s.maskSet[slot]; w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &tb;
        w[1].dstSet = s.compSet[slot]; w[1].dstBinding = 0;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &mi;
        w[2].dstSet = s.compSet[slot]; w[2].dstBinding = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &tb;
        vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
        s.setGen[slot] = s.gen;
    }

    // ---- a) the mask ----
    {
        // The previous edge pass read the tiles (vertex) and the mask (fragment).
        const VkMemoryBarrier2 war = memBarrier(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        barrier(cmd, 1, &war, nullptr);
        vkCmdFillBuffer(cmd, s.tiles.buffer, 0, VK_WHOLE_SIZE, 0u);
        const VkMemoryBarrier2 mb[2] = {
            memBarrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT),
            // the scene / HUD depth as the passes before left it
            memBarrier(VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT)};
        const VkImageMemoryBarrier2 ib = imgBarrier(s.maskImg.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        barrier(cmd, 2, mb, &ib);

        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = s.maskImg.view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.uint32[0] = 0u;
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = depth.view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;   // read only
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width, height}}; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        bindDrawSets(cmd, dsets);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.maskLayout, 2, 1, &s.maskSet[slot], 0, nullptr);
        vkCmdBindIndexBuffer(cmd, mIbo.buffer, 0, VK_INDEX_TYPE_UINT16);
        VkBuffer cmds = mSelCmds[mFrameSlot & 1u].buffer;
        const float tile_n = (float)(s.tilesX * s.tilesY);
        // Hidden pass first (occluded bit), the visible pass over it -- both always: the
        // contour of an object half behind a wall needs its hidden half to know where it is
        // not an edge; whether occluded lines show is the edge pass's choice.
        auto drawLists = [&](uint32_t first_list, VkDeviceSize first_cmd, int pair, bool rigged)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                bindGraphicsPipeline(cmd, s.mask[pair * 2 + pass]);
                VkDeviceSize off = first_cmd;
                for (uint32_t k = 0; k < SEL_KINDS; ++k)
                {
                    const uint32_t n = mSelCount[first_list + k];
                    if (n)
                    {
                        VKDrawPush p = push;
                        if (rigged)
                        {   // skinned, eye-relative verts: the translation-free view-projection
                            std::memcpy(p.view_proj, mViewProjRel, sizeof(p.view_proj));
                            p.ambient_color[3] = selfHideW();
                        }
                        else if (!hud)
                        {   // camera-relative like the main view's draws (#555): same depths
                            std::memcpy(p.view_proj, mViewProjRel, sizeof(p.view_proj));
                            p.origin[0] = push.cam_time[0]; p.origin[1] = push.cam_time[1];
                            p.origin[2] = push.cam_time[2]; p.origin[3] = 0.f;
                        }
                        p.water_fog[0] = (float)((k + 1u) | (pass == 0 ? 4u : 0u));
                        p.water_fog[1] = (float)s.tilesX;
                        p.water_fog[2] = tile_n;
                        p.water_fog[3] = 0.f;
                        pushDrawView(cmd, s.maskLayout, p);
                        vkCmdDrawIndexedIndirect(cmd, cmds, off * kDrawCmdBytes, n, (uint32_t)kDrawCmdBytes);
                    }
                    off += n;
                }
            }
        };
        VkDeviceSize voff = 0;
        const uint32_t rig = 2 * SEL_KINDS;
        uint32_t rig_n = 0;
        for (uint32_t l = rig; l < SEL_LISTS; ++l) rig_n += mSelCount[l];
        if (!hud && rig_n && mSkinVbo.buffer != VK_NULL_HANDLE)
        {
            VkDeviceSize first = 0;
            for (uint32_t l = 0; l < rig; ++l) first += mSelCount[l];
            vkCmdBindVertexBuffers(cmd, 0, 1, &mSkinVbo.buffer, &voff);
            drawLists(rig, first, 2, true);
        }
        const uint32_t base = hud ? SEL_KINDS : 0u;
        VkDeviceSize first = 0;
        for (uint32_t l = 0; l < base; ++l) first += mSelCount[l];
        vkCmdBindVertexBuffers(cmd, 0, 1, &mVbo.buffer, &voff);
        drawLists(base, first, hud ? 1 : 0, false);
        vkCmdEndRendering(cmd);
    }

    // ---- b) the edge pass over the marked tiles, blended over the frame ----
    {
        const VkMemoryBarrier2 mb[2] = {
            memBarrier(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT),
            memBarrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)};
        const VkImageMemoryBarrier2 ib = imgBarrier(s.maskImg.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, 2, mb, &ib);

        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = colorView; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {width, height}}; ri.layerCount = 1;
        ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
        vkCmdBeginRendering(cmd, &ri);
        const VkViewport full{0.f, 0.f, (float)width, (float)height, 0.f, 1.f};
        vkCmdSetViewport(cmd, 0, 1, &full);   // tile quads are in display pixels
        vkCmdSetScissor(cmd, 0, 1, &sc);
        bindGraphicsPipeline(cmd, s.composite);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compLayout, 0, 1, &s.compSet[slot], 0, nullptr);
        SelOutlinePush op{};
        std::memcpy(op.colors[0], mSelParentColor, sizeof(op.colors[0]));
        std::memcpy(op.colors[1], mSelChildColor, sizeof(op.colors[1]));
        std::memcpy(op.colors[2], mSelHoverColor, sizeof(op.colors[2]));
        op.dims[0] = (float)s.tilesX; op.dims[1] = (float)s.tilesY;
        op.dims[2] = (float)width;    op.dims[3] = (float)height;
        // ~2 px at a 1080-line display, scaled with the display (max 4: the tile neighbourhood
        // the vertex stage checks covers 16 px).
        op.params[0] = (float)llclamp((S32)std::lround(2.0 * height / 1080.0), 1, 4);
        op.params[1] = mSelHidden ? 1.f : 0.f;
        op.params[2] = kOccludedAlpha;
        vkCmdPushConstants(cmd, s.compLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(op), &op);
        vkCmdDraw(cmd, 6, s.tilesX * s.tilesY, 0, 0);
        vkCmdEndRendering(cmd);
        const VkMemoryBarrier2 after = memBarrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                                      | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        barrier(cmd, 1, &after, nullptr);
    }
    gpuSubMark(cmd, hud ? "g_gmdx_sel_outline_hud" : "g_gmdx_sel_outline");
}

void LLCleanRendererVK::destroySelOutline()
{
    if (!mSelOutline) return;
    GMDXSelOutlineVK& s = *mSelOutline;
    if (mCtx)
    {
        VkDevice dev = mCtx->device();
        vkDeviceWaitIdle(dev);
        for (VkPipeline& p : s.classic) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        for (VkPipeline& p : s.mask) { if (p) vkDestroyPipeline(dev, p, nullptr); p = VK_NULL_HANDLE; }
        if (s.composite) vkDestroyPipeline(dev, s.composite, nullptr);
        if (s.maskLayout) vkDestroyPipelineLayout(dev, s.maskLayout, nullptr);
        if (s.compLayout) vkDestroyPipelineLayout(dev, s.compLayout, nullptr);
        s.pool.destroy();
        if (s.maskDsl) vkdDestroySetLayout(dev, s.maskDsl);
        if (s.compDsl) vkdDestroySetLayout(dev, s.compDsl);
        if (s.sampler) vkDestroySampler(dev, s.sampler, nullptr);
        if (s.maskImg.image) mCtx->destroyImage(s.maskImg);
        if (s.tiles.buffer) mCtx->destroyBuffer(s.tiles);
    }
    delete mSelOutline;
    mSelOutline = nullptr;
}
