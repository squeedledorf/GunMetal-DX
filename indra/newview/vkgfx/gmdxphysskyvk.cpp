/**
 * @file gmdxphysskyvk.cpp
 * @brief GunMetal-DX physical sky (Hillaire 2020) for the Vulkan clean renderer.
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

// <GMDX> PHYSICAL SKY (RenderVKPhysicalSky, on by default; off = Classic, the WindLight sky).
// Defines the LLCleanRendererVK members declared in its "PHYSICAL SKY" block; the shaders are
// vkclean/shaders/clean_physky.glsl (maths, after Epic Games' MIT-licensed reference) and
// clean_physky_lut.comp, read in clean_atmos_common.glsl.
//
// EEP's physical layers (Rayleigh / Mie / absorption configs) are what nobody tunes; every
// region and preset was made with the legacy WindLight haze. So the atmosphere is DERIVED from
// those values, keeping each sky's intent:
//   blue_density       -> Rayleigh scattering, per channel against EEP's default (default sky =
//                         Earth's Rayleigh coefficients)
//   haze_density       -> Mie scattering (default 0.7 = Earth's)
//   density_multiplier -> both, by the square root of its ratio to the default (1e-4)
//   max_y              -> the Mie scale height (default 1605 m = Earth's 1.2 km)
//   sun / moon light   -> the light's illuminance, its EEP colour kept, times
//                         RenderVKPhysicalSkyExposure
//   blue_horizon       -> a tint on what the Rayleigh air scatters (not on what it absorbs), per
//                         channel against EEP's default: the sky's colour as the artist set it
//   haze_horizon       -> a tint on what the Mie haze scatters (default 0.19 = none)
//   ambient            -> light scattered toward the viewer, times RenderVKPhysicalSkyAmbient
// Distant scenery fades into that sky by the physical air's extinction, stretched by
// RenderVKPhysicalSkyAerial (real air is nearly clear over SL's draw distances), on top of
// WindLight's haze; clouds and god rays are lit through the physical air too.
// The sky's luminance then rolls off softly (RenderVKPhysicalSkyHighlight) so a low sun's glow
// doesn't burn the horizon white.
// The viewer's height is the camera's altitude (<GMDX> lifted toward near space above GMDXSpaceStart:
// gmdxspacealt.h). Only the sky's colour changes: the haze
// amount and every surface's lighting stay WindLight's (clean_atmos_common.glsl).
//
// Tables (RGBA16F, GENERAL, made once in writeDescriptors): transmittance 256 x 64 and multiple
// scattering 32 x 32, rebuilt when the medium changes (a day cycle's blend: often); sky view
// 192 x 108, every frame while the mode is on. Under 0.2 ms together on a mid-range GPU.
//
// TWO LIGHTS (GMDXPhysicalNight, gmdxnightlight.h): the light is then the SUN, always (below the
// horizon too: its twilight comes through the planet's shadow and the multiple scattering), and
// the moon is a second light: the sky-view table is 192 x 216, the sun's 108 rows on top and the
// moon's below, both lit at the sun's illuminance (fp16-safe) -- the shaders weigh the moon's half
// by its strength (frame info gmdx_misc.w, clean_atmos_common.glsl physkySkyRaw). The moon's half
// is only made while its light counts: physky[4].w = 2 (1 = the sun alone). The transmittance and
// multiple-scattering tables are light-independent and serve both.

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llviewercontrol.h"
#include "../gmdxnightlight.h"   // <GMDX> physical night: the moon as a second light
#include "../gmdxspacealt.h"     // <GMDX> space at altitude

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr uint32_t kTransW = 256, kTransH = 64, kMsRes = 32, kViewW = 192, kViewH = 108;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Earth (Hillaire's defaults), per km.
constexpr float kEarthRayleigh[3] = {5.802e-3f, 13.558e-3f, 33.1e-3f};
constexpr float kEarthMieScat = 3.996e-3f, kEarthMieExt = 4.440e-3f, kMieG = 0.8f;
constexpr float kBottomKm = 6360.f, kTopKm = 6460.f, kRayleighH = 8.f, kMieH = 1.2f, kGroundAlbedo = 0.3f;
// EEP's legacy haze defaults (LLSettingsSky).
constexpr float kDefBlueDensity[3] = {0.2447f, 0.4487f, 0.7599f};
constexpr float kDefHazeDensity = 0.7f, kDefDensityMul = 0.0001f, kDefMaxY = 1605.f;
constexpr float kDefBlueHorizon[3] = {0.4954f, 0.4954f, 0.6399f}, kDefHazeHorizon = 0.19f;

VkShaderModule pkModule(VkDevice dev, const uint32_t* code, size_t bytes, const char* name)
{
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkNoteShaderModule(m, code, bytes, name);
    return m;
}

float srgbToLinear(float cs)
{
    return cs <= 0.04045f ? cs / 12.92f : std::pow((std::max(cs, 0.0f) + 0.055f) / 1.055f, 2.4f);
}

void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 m{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    m.srcStageMask = ss; m.srcAccessMask = sa; m.dstStageMask = ds; m.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}
} // namespace

void LLCleanRendererVK::physSkyFill(VKSSRInfo& si)
{
    static LLCachedControl<bool> on_setting(gSavedSettings, "RenderVKPhysicalSky", true);
    static LLCachedControl<F32> exposure(gSavedSettings, "RenderVKPhysicalSkyExposure", 8.f);
    static LLCachedControl<F32> ambient_scale(gSavedSettings, "RenderVKPhysicalSkyAmbient", 0.3f);
    static LLCachedControl<F32> highlight(gSavedSettings, "RenderVKPhysicalSkyHighlight", 1.5f);
    static LLCachedControl<F32> aerial(gSavedSettings, "RenderVKPhysicalSkyAerial", 6.f);

    std::memset(si.physky, 0, sizeof(si.physky));
    // <GMDX> SPACE AT ALTITUDE (gmdxspacealt.h): this frame's amount, for every sky (frame info
    // cluster_rt.w, unused upstream). LL's random stars (physical night off) show by day up there.
    const float space = gmdxSpaceUpdate(si.cam_origin[2]);
    si.cluster_rt[3] = space;
    if (space > 0.f) si.bodies[7][0] = std::max(si.bodies[7][0], 0.5f * space);
    const float* a = si.atmos;
    auto v = [&](int row, int c) { return a[row * 4 + c]; };
    const bool on = on_setting && v(6, 2) > 0.5f;   // (no sky this frame: WindLight's own off path)
    if (!on)
    {
        std::memset(mPkPush, 0, sizeof(mPkPush));
        return;
    }

    const float dens = std::clamp(std::sqrt(std::max(v(4, 3), 1e-7f) / kDefDensityMul), 0.25f, 4.f);
    const float mie = std::clamp(v(1, 3) / kDefHazeDensity, 0.f, 8.f) * dens;
    for (int c = 0; c < 3; ++c)
    {
        si.physky[0][c] = kEarthRayleigh[c] * std::clamp(v(1, c) / kDefBlueDensity[c], 0.f, 6.f) * dens;
        si.physky[1][c] = kEarthMieScat * mie;
        si.physky[2][c] = kEarthMieExt * mie;
    }
    si.physky[0][3] = kBottomKm;
    si.physky[1][3] = kTopKm;
    si.physky[2][3] = kMieG;

    // The light: the sun's or moon's EEP colour (the transmittance LUT reddens it further low down).
    // Physical night: the sun's own (atmos[8] / [10]), the moon a second light (see the header).
    const GMDXNightLight& night = gmdxNightLight();
    const int lrow = night.on ? 8 : 2, drow = night.on ? 10 : 7;
    for (int c = 0; c < 3; ++c) si.physky[3][c] = srgbToLinear(std::max(v(lrow, c), 0.f)) * exposure;
    si.physky[3][3] = std::clamp(si.cam_origin[2] * 0.001f, 0.f, 60.f);   // viewer height (km)
    // <GMDX> space at altitude: the sky's viewer rises into the thin air (clouds keep the real height)
    if (space > 0.f) si.physky[3][3] = std::min(si.physky[3][3] + space * kGmdxSpaceKm, 60.f);

    float lx = v(drow, 0), ly = v(drow, 1), lz = v(drow, 2);
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    if (ll > 1e-6f) { lx /= ll; ly /= ll; lz /= ll; } else { lx = 0.f; ly = 0.f; lz = 1.f; }
    si.physky[4][0] = lx; si.physky[4][1] = ly; si.physky[4][2] = lz;
    si.physky[4][3] = 1.f;
    // The moon's half of the sky-view table: while its light counts (well below the horizon its
    // sky is the planet's shadow; a new or faint moon is nothing next to the stars).
    if (night.on && night.moon > 1e-6f)
    {
        const float mz = v(11, 2) / std::max(std::sqrt(v(11, 0) * v(11, 0) + v(11, 1) * v(11, 1) + v(11, 2) * v(11, 2)), 1e-6f);
        if (mz > -0.2f) si.physky[4][3] = 2.f;
    }

    // Physical night: EEP's ambient is no light in the sky, and a night sky's is set for Second Life's
    // full-strength moon; scattered as it is, it made up most of the night sky away from the moon (a
    // flat glow the moonlit clouds could not stand out from). It follows the WindLight sky's own light:
    // the sun's while it is up (as before), the moon's strength at night.
    const float amb_k = night.on ? std::clamp(std::max(night.sky_q, night.moon), 0.f, 1.f) : 1.f;
    for (int c = 0; c < 3; ++c) si.physky[5][c] = srgbToLinear(std::max(v(3, c), 0.f)) * ambient_scale * amb_k;
    si.physky[5][3] = -1.f / kRayleighH;
    const float mie_h = std::clamp(kMieH * v(3, 3) / kDefMaxY, 0.2f, 6.f);
    si.physky[6][0] = -1.f / mie_h;
    si.physky[6][1] = kGroundAlbedo;
    si.physky[6][2] = std::max(0.f, (float)highlight);
    si.physky[6][3] = std::max(0.f, (float)aerial);
    // The artist's tints (capped: past ~2 the multiple-scattering series stops converging).
    for (int c = 0; c < 3; ++c)
        si.physky[7][c] = std::clamp(v(0, c) / kDefBlueHorizon[c], 0.f, 2.f);
    si.physky[7][3] = std::clamp(std::sqrt(std::max(v(0, 3), 0.f) / kDefHazeHorizon), 0.f, 2.f);

    std::memcpy(mPkPush, si.physky, sizeof(mPkPush));
}

void LLCleanRendererVK::physSkyInit()
{
    VkDevice dev = mCtx->device();
    if (!mPkTrans.image)
    {
        VkMemScope mem(VKMEM_TARGET);
        const VkImageUsageFlags use = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        mPkTrans = mCtx->createImage2D(kTransW, kTransH, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);
        mPkMs = mCtx->createImage2D(kMsRes, kMsRes, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);
        mPkView = mCtx->createImage2D(kViewW, kViewH * 2, kLutFormat, use, VK_IMAGE_ASPECT_COLOR_BIT);   // <GMDX> sun / moon halves
        mPkLayoutsReady = false;
        mPkMediumValid = false;
    }
    if (!mPkSampler)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &mPkSampler));
    }
    if (!mPkTrans.view || !mPkView.view || !mPkSampler) return;
    VkDescriptorImageInfo ii[2]{};
    ii[0] = {mPkSampler, mPkView.view, VK_IMAGE_LAYOUT_GENERAL};
    ii[1] = {mPkSampler, mPkTrans.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i)
    {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = mDsetDraw0; w[i].dstBinding = 57 + i; w[i].descriptorCount = 1;   // <GMDX> 57 / 58 (46 / 47 are upstream's clouds, 54 / 55 its VS instances)
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
}

void LLCleanRendererVK::physSkyRecord(VkCommandBuffer cmd)
{
    if (!mPkTrans.image || !mPkMs.image || !mPkView.image) return;
    VkDevice dev = mCtx->device();

    // The images leave UNDEFINED once, mode on or off: draw set 0 describes them as GENERAL.
    if (!mPkLayoutsReady)
    {
        VkImageMemoryBarrier2 b[3]{};
        const VkImage imgs[3] = {mPkTrans.image, mPkMs.image, mPkView.image};
        for (int i = 0; i < 3; ++i)
        {
            b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE; b[i].srcAccessMask = 0;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].image = imgs[i]; b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 3; d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
        mPkLayoutsReady = true;
    }

    if (mPkPush[4][3] < 0.5f) return;   // mode off (physSkyFill)

    if (!mPkPipe[0] && !mPkPipeFailed)
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 3; lci.pBindings = b;
        VK_CHECK(vkdCreateSetLayout(dev, &lci, &mPkDsl, "mDslPhysSky"));
        VK_CHECK(mPkPool.create(dev, "physsky", {{mPkDsl, 3}}));
        for (int i = 0; i < 3; ++i)
            VKD_ALLOC_SOFT(mPkPool.allocate(mPkDsl, &mPkSet[i]), { mPkPipeFailed = true; return; });

        // Set i writes its table (0 trans, 1 MS, 2 view) and reads the transmittance + MS ones.
        const VkImageView outs[3] = {mPkTrans.view, mPkMs.view, mPkView.view};
        for (int i = 0; i < 3; ++i)
        {
            VkDescriptorImageInfo oi{VK_NULL_HANDLE, outs[i], VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo ti{mPkSampler, mPkTrans.view, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorImageInfo mi{mPkSampler, mPkMs.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w[3]{};
            for (uint32_t j = 0; j < 3; ++j)
            {
                w[j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[j].dstSet = mPkSet[i]; w[j].dstBinding = j; w[j].descriptorCount = 1;
                w[j].descriptorType = j == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            w[0].pImageInfo = &oi; w[1].pImageInfo = &ti; w[2].pImageInfo = &mi;
            vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);
        }

        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mPkPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &mPkDsl;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        VK_CHECK(vkdCreatePipelineLayout(dev, &pli, &mPkLayout, "mPkLayout"));
        const uint32_t* code[3] = {kWorldPhysSkyTransSpv, kWorldPhysSkyMsSpv, kWorldPhysSkyViewSpv};
        const size_t bytes[3] = {kWorldPhysSkyTransSpv_bytes, kWorldPhysSkyMsSpv_bytes, kWorldPhysSkyViewSpv_bytes};
        const char* names[3] = {"kWorldPhysSkyTransSpv", "kWorldPhysSkyMsSpv", "kWorldPhysSkyViewSpv"};
        for (int i = 0; i < 3; ++i)
        {
            VkShaderModule m = pkModule(dev, code[i], bytes[i], names[i]);
            mPkPipe[i] = m ? createComputePipeline(dev, m, mPkLayout) : VK_NULL_HANDLE;
            if (m) vkDestroyShaderModule(dev, m, nullptr);
            if (!mPkPipe[i]) mPkPipeFailed = true;
        }
        if (mPkPipeFailed)
        {
            LL_WARNS("VKPhysSky") << "physical sky pipelines failed to build; the WindLight sky stays" << LL_ENDL;
            return;
        }
        LL_INFOS("VKPhysSky") << "physical sky pipelines built" << LL_ENDL;
    }
    if (mPkPipeFailed) return;

    // Last frame's readers (sky / lighting FS, computes) before this frame's writes.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdPushConstants(cmd, mPkLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mPkPush), mPkPush);

    // The medium: Rayleigh, Mie (scattering, extinction, g), scale heights, radii.
    float key[16];
    for (int c = 0; c < 4; ++c)
    {
        key[c] = mPkPush[0][c]; key[4 + c] = mPkPush[1][c]; key[8 + c] = mPkPush[2][c]; key[12 + c] = mPkPush[7][c];
    }
    key[3] += mPkPush[5][3] * 1000.f + mPkPush[6][0] * 7.f + mPkPush[6][1] * 13.f;   // (scale heights, albedo)
    if (!mPkMediumValid || std::memcmp(key, mPkMediumKey, sizeof(key)) != 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[0]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[0], 0, nullptr);
        vkCmdDispatch(cmd, kTransW / 8, kTransH / 8, 1);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[1]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[1], 0, nullptr);
        vkCmdDispatch(cmd, kMsRes, kMsRes, 1);
        memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        std::memcpy(mPkMediumKey, key, sizeof(key));
        mPkMediumValid = true;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkPipe[2]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPkLayout, 0, 1, &mPkSet[2], 0, nullptr);
    vkCmdDispatch(cmd, (kViewW + 7) / 8, (kViewH + 7) / 8, 1);
    // <GMDX> Physical night: the moon's half (rows kViewH..), the same light from the moon's
    // direction (atmos[11]); [4].w = 3 tells the pass which half. No barrier between: disjoint rows.
    if (mPkPush[4][3] > 1.5f)
    {
        float moonPush[8][4];
        std::memcpy(moonPush, mPkPush, sizeof(moonPush));
        const float* m = &mAtmos[11 * 4];
        const float ml = std::max(std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), 1e-6f);
        for (int c = 0; c < 3; ++c) moonPush[4][c] = m[c] / ml;
        moonPush[4][3] = 3.f;
        vkCmdPushConstants(cmd, mPkLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(moonPush), moonPush);
        vkCmdDispatch(cmd, (kViewW + 7) / 8, (kViewH + 7) / 8, 1);
    }
    // To every reader: the sky, probe and lighting FS, and the computes that run the atmosphere.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLCleanRendererVK::physSkyDestroy()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (int i = 0; i < 3; ++i)
    {
        if (mPkPipe[i]) vkDestroyPipeline(dev, mPkPipe[i], nullptr);
        mPkPipe[i] = VK_NULL_HANDLE;
        mPkSet[i] = VK_NULL_HANDLE;
    }
    if (mPkLayout) vkDestroyPipelineLayout(dev, mPkLayout, nullptr);
    mPkLayout = VK_NULL_HANDLE;
    mPkPool.destroy();
    if (mPkDsl) vkdDestroySetLayout(dev, mPkDsl);
    mPkDsl = VK_NULL_HANDLE;
    if (mPkSampler) vkDestroySampler(dev, mPkSampler, nullptr);
    mPkSampler = VK_NULL_HANDLE;
    if (mPkTrans.image) mCtx->destroyImage(mPkTrans);
    if (mPkMs.image) mCtx->destroyImage(mPkMs);
    if (mPkView.image) mCtx->destroyImage(mPkView);
    mPkTrans = mPkMs = mPkView = GpuImage{};
    mPkLayoutsReady = mPkMediumValid = mPkPipeFailed = false;
}
