/**
 * @file gmdxrezfxvk.cpp
 * @brief GunMetal-DX rez effects, the Vulkan renderer's side: the effect table upload.
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

// <GMDX> See gmdxrezfx.h. The Tron wireframe's lit FS twins live in this file only (the
// renderer and the spec sets ask for them by name). The effect table (draw set 0 binding 45,
// made in writeDescriptors) is updated in the command buffer (vkCmdUpdateBuffer, as the terrain
// bake table), so every frame's draws read that frame's rows whatever the frames in flight.

#include "linden_common.h"

#include "llcleanrenderervk.h"
#include "vk_world_shaders_spv.h"   // (only the wireframe twins are referenced here)
#include "../gmdxrezfx.h"

#include <algorithm>
#include <string>
#include <vector>

void GMDXRezFx::wireModule(const char* name, const uint32_t*& code, size_t& bytes)
{
    if (!name || !wireframe()) return;
    // "kWorldFragSpv" (moduleFromSpv) and "Frag" (the spec sets) name the same module.
    std::string n(name);
    if (n.rfind("kWorld", 0) == 0) n.erase(0, 6);
    if (n.size() > 3 && n.compare(n.size() - 3, 3, "Spv") == 0) n.resize(n.size() - 3);
    static const struct { const char* name; const uint32_t* code; size_t bytes; } k[] = {
        {"Frag",        kWorldFragWireSpv,        kWorldFragWireSpv_bytes},
        {"FragEz",      kWorldFragEzWireSpv,      kWorldFragEzWireSpv_bytes},
        {"FragRt",      kWorldFragRtWireSpv,      kWorldFragRtWireSpv_bytes},
        {"FragRtEz",    kWorldFragRtEzWireSpv,    kWorldFragRtEzWireSpv_bytes},
        {"FragBlend",   kWorldFragBlendWireSpv,   kWorldFragBlendWireSpv_bytes},
        {"FragBlendRt", kWorldFragBlendRtWireSpv, kWorldFragBlendRtWireSpv_bytes},
        {"FragGbuf",    kWorldFragGbufWireSpv,    kWorldFragGbufWireSpv_bytes},
        {"FragGbufNt",  kWorldFragGbufNtWireSpv,  kWorldFragGbufNtWireSpv_bytes}};
    for (const auto& e : k)
        if (n == e.name)
        {
            code = e.code;
            bytes = e.bytes;
            return;
        }
}

void LLCleanRendererVK::rezFxRecord(VkCommandBuffer cmd)
{
    if (!mRezFxTable.buffer) return;
    U32 count = 0, gen = 0;
    const GMDXRezFx::Slot* rows = GMDXRezFx::table(count, gen);
    if (mRezFxValid && gen == mRezFxGen) return;   // nothing changed (no effect running): no work
    // WAR: earlier frames' reads (VS / FS) -> these updates; then the updates -> this frame's draws.
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.buffer = mRezFxTable.buffer; b.size = VK_WHOLE_SIZE;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d.bufferMemoryBarrierCount = 1; d.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
    // The first time the whole table is zeroed (no slot names an effect); then the live range.
    // (<= 511 x 80 B: under vkCmdUpdateBuffer's 64 KB)
    if (!mRezFxValid)
    {
        std::vector<GMDXRezFx::Slot> all(GMDXRezFx::MAX_SLOTS, GMDXRezFx::Slot{});
        std::copy(rows, rows + count, all.begin());
        vkCmdUpdateBuffer(cmd, mRezFxTable.buffer, 0, (VkDeviceSize)all.size() * sizeof(GMDXRezFx::Slot), all.data());
        mRezFxValid = true;
    }
    else if (count)
        vkCmdUpdateBuffer(cmd, mRezFxTable.buffer, 0, (VkDeviceSize)count * sizeof(GMDXRezFx::Slot), rows);
    mRezFxGen = gen;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &d);
}
