/**
 * @file llcleanrtomm.cpp
 * @brief See llcleanrtomm.h.
 */
#include "linden_common.h"

#include "llcleanrtomm.h"
#include "vk_descpool.h"   // vkdCreatePipelineLayout (push / descriptor limit check)
#include "vk_pipeline.h"
#include "vk_world_shaders_spv.h"

#include "llerror.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <sstream>
#include <vector>

namespace
{
uint32_t extractEvenBits(uint32_t x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return x;
}

uint32_t prefixEor(uint32_t x)
{
    x ^= x >> 1; x ^= x >> 2; x ^= x >> 4; x ^= x >> 8;
    return x;
}

// Distance along the curve -> discrete barycentrics of the micro-triangle.
void index2dbary(uint32_t index, uint32_t& u, uint32_t& v, uint32_t& w)
{
    const uint32_t b0 = extractEvenBits(index);
    const uint32_t b1 = extractEvenBits(index >> 1);
    const uint32_t fx = prefixEor(b0);
    const uint32_t fy = prefixEor(b0 & ~b1);
    const uint32_t t = fy ^ b1;
    u = (fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t);
    v = fy ^ b0;
    w = (~fx & ~t) | (b0 & ~t) | (~b0 & fx & t);
}
} // namespace

void ommIndexToBary(uint32_t index, uint32_t level, float uv[3][2])
{
    if (level == 0)
    {
        uv[0][0] = 0.f; uv[0][1] = 0.f; uv[1][0] = 1.f; uv[1][1] = 0.f; uv[2][0] = 0.f; uv[2][1] = 1.f;
        return;
    }
    uint32_t iu, iv, iw;
    index2dbary(index, iu, iv, iw);
    const uint32_t m = (1u << level) - 1u;
    iu &= m; iv &= m; iw &= m;
    const bool upright = ((iu & 1u) ^ (iv & 1u) ^ (iw & 1u)) != 0u;
    if (!upright) { iu += 1u; iv += 1u; }
    const float s = 1.f / (float)(1u << level);
    const float u = (float)iu * s, v = (float)iv * s;
    uv[0][0] = u; uv[0][1] = v;
    if (upright) { uv[1][0] = u + s; uv[1][1] = v;     uv[2][0] = u;     uv[2][1] = v + s; }
    else         { uv[1][0] = u - s; uv[1][1] = v;     uv[2][0] = u;     uv[2][1] = v - s; }
}

namespace
{
// One self-test case: N triangles, triangle k's micromap marks ONLY micro-triangle k
// opaque (k < 4^level_k). prod = the production shape: 4-state data, the micromap at a
// non-zero offset of a buffer that also holds the BLAS, indexed UINT32 geometry with the
// renderer's 48-byte vertex stride.
bool selfTestCase(VkDeviceContext& ctx, const std::vector<uint32_t>& levels, bool prod, std::string& detail)
{
    VkDevice dev = ctx.device();
    const auto& rt = ctx.rt();
    const uint32_t N = (uint32_t)levels.size();
    uint32_t maxMicro = 1;
    for (uint32_t L : levels) maxMicro = std::max(maxMicro, 1u << (2u * L));
    const VkOpacityMicromapFormatEXT fmt = prod ? VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT : VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT;
    auto bytesFor = [&](uint32_t L) { const uint32_t m = 1u << (2u * L); return std::max(1u, prod ? m / 4u : m / 8u); };
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkBufferUsageFlags ommIn = VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT;
    const VkBufferUsageFlags asIn = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    VkMemScope memcat(VKMEM_RT_AUX);   // #407 (OMM self-test, transient)
    std::deque<GpuBuffer> bufs;   // stable references (mk returns one)
    auto mk = [&](VkDeviceSize sz, VkBufferUsageFlags u, VkMemoryPropertyFlags p, bool addr) -> GpuBuffer& {
        bufs.push_back(ctx.createBuffer(std::max<VkDeviceSize>(sz, 16), u, p, addr));
        return bufs.back();
    };
    VkMicromapEXT mm = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE, tlas = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool ok = false;
    do
    {
        // ---- micromap inputs ----
        std::vector<uint32_t> off(N);
        uint32_t dataBytes = 0;
        uint32_t perLevel[13] = {};
        for (uint32_t k = 0; k < N; ++k) { off[k] = dataBytes; dataBytes += bytesFor(levels[k]); ++perLevel[levels[k]]; }
        GpuBuffer& data = mk(dataBytes, ommIn, host, true);
        std::memset(data.mapped, 0, dataBytes);
        for (uint32_t k = 0; k < N; ++k)
        {
            uint8_t* d = static_cast<uint8_t*>(data.mapped) + off[k];
            if (prod) d[k / 4u] |= (uint8_t)(1u << ((k % 4u) * 2u));   // state 1 = opaque
            else      d[k / 8u] |= (uint8_t)(1u << (k % 8u));
        }
        GpuBuffer& tri = mk((VkDeviceSize)N * sizeof(VkMicromapTriangleEXT), ommIn, host, true);
        auto* t = static_cast<VkMicromapTriangleEXT*>(tri.mapped);
        for (uint32_t k = 0; k < N; ++k)
        {
            t[k].dataOffset = off[k];
            t[k].subdivisionLevel = (uint16_t)levels[k];
            t[k].format = (uint16_t)fmt;
        }
        std::vector<VkMicromapUsageEXT> usage;
        for (uint32_t l = 0; l < 13; ++l) if (perLevel[l]) usage.push_back(VkMicromapUsageEXT{perLevel[l], l, (uint32_t)fmt});
        VkMicromapBuildInfoEXT mb{VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT};
        mb.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        mb.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        mb.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        mb.usageCountsCount = (uint32_t)usage.size(); mb.pUsageCounts = usage.data();
        VkMicromapBuildSizesInfoEXT ms{VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT};
        rt.vkGetMicromapBuildSizesEXT(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mb, &ms);
        if (!ms.micromapSize) { detail = "micromap size 0"; break; }
        GpuBuffer& mmScr = mk(ms.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, local, true);

        // ---- BLAS geometry: triangle k = (2k,0,0), (2k+1,0,0), (2k,1,0) ----
        const uint32_t stride = prod ? 48u : 12u;
        GpuBuffer& vtx = mk((VkDeviceSize)N * 3 * stride, asIn, host, true);
        std::memset(vtx.mapped, 0, (size_t)N * 3 * stride);
        for (uint32_t k = 0; k < N; ++k)
        {
            const float x = 2.f * (float)k;
            const float p[3][3] = {{x, 0, 0}, {x + 1.f, 0, 0}, {x, 1.f, 0}};
            for (int v = 0; v < 3; ++v)
                std::memcpy(static_cast<uint8_t*>(vtx.mapped) + (size_t)(k * 3 + v) * stride, p[v], 12);
        }
        GpuBuffer& ibo = mk((VkDeviceSize)N * 3 * 4, asIn, host, true);
        for (uint32_t i = 0; i < N * 3; ++i) static_cast<uint32_t*>(ibo.mapped)[i] = i;
        GpuBuffer& ommIdx = mk((VkDeviceSize)N * 4, asIn, host, true);
        for (uint32_t k = 0; k < N; ++k) static_cast<uint32_t*>(ommIdx.mapped)[k] = k;
        VkAccelerationStructureTrianglesOpacityMicromapEXT om{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT};
        om.indexType = VK_INDEX_TYPE_UINT32;
        om.indexBuffer.deviceAddress = ommIdx.address;
        om.indexStride = 4;
        om.usageCountsCount = (uint32_t)usage.size(); om.pUsageCounts = usage.data();
        VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        auto& tr = g.geometry.triangles;
        tr = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        tr.pNext = &om;
        tr.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tr.vertexData.deviceAddress = vtx.address;
        tr.vertexStride = stride;
        tr.maxVertex = N * 3 - 1;
        tr.indexType = prod ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR;
        if (prod) tr.indexData.deviceAddress = ibo.address;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                 | (prod ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT : 0);
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1; bi.pGeometries = &g;
        const uint32_t prims = N;
        VkAccelerationStructureBuildSizesInfoKHR bs{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &bs);

        // Storage: prod = ONE buffer holding (pad 256) micromap | BLAS, like the RT arena.
        auto al = [](VkDeviceSize v) { return (v + 255) & ~VkDeviceSize(255); };
        VkBuffer mmBuffer, asBuffer;
        VkDeviceSize mmOff = 0, asOff = 0;
        if (prod)
        {
            mmOff = 256; asOff = al(mmOff + ms.micromapSize);
            GpuBuffer& shared = mk(asOff + bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                                   | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, local, true);
            mmBuffer = asBuffer = shared.buffer;
        }
        else
        {
            mmBuffer = mk(ms.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT, local, false).buffer;
            asBuffer = mk(bs.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, local, true).buffer;
        }
        VkMicromapCreateInfoEXT mci{VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT};
        mci.buffer = mmBuffer; mci.offset = mmOff; mci.size = ms.micromapSize; mci.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        if (rt.vkCreateMicromapEXT(dev, &mci, nullptr, &mm) != VK_SUCCESS) { detail = "vkCreateMicromapEXT failed"; break; }
        mb.dstMicromap = mm;
        mb.data.deviceAddress = data.address;
        mb.triangleArray.deviceAddress = tri.address;
        mb.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        mb.scratchData.deviceAddress = (mmScr.address + 255) & ~VkDeviceAddress(255);
        om.micromap = mm;
        GpuBuffer& blasScr = mk(bs.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, local, true);
        VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        aci.buffer = asBuffer; aci.offset = asOff; aci.size = bs.accelerationStructureSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (rt.vkCreateAccelerationStructureKHR(dev, &aci, nullptr, &blas) != VK_SUCCESS) { detail = "BLAS create failed"; break; }
        bi.dstAccelerationStructure = blas;
        bi.scratchData.deviceAddress = (blasScr.address + 255) & ~VkDeviceAddress(255);
        VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        dai.accelerationStructure = blas;
        const VkDeviceAddress blasAddr = rt.vkGetAccelerationStructureDeviceAddressKHR(dev, &dai);

        // ---- TLAS: one identity instance, no opacity overrides ----
        GpuBuffer& inst = mk(sizeof(VkAccelerationStructureInstanceKHR), asIn, host, true);
        VkAccelerationStructureInstanceKHR ii{};
        ii.transform.matrix[0][0] = ii.transform.matrix[1][1] = ii.transform.matrix[2][2] = 1.f;
        ii.mask = 0xFF;
        ii.flags = prod ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : 0;
        ii.accelerationStructureReference = blasAddr;
        std::memcpy(inst.mapped, &ii, sizeof(ii));
        VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tg.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        tg.geometry.instances.data.deviceAddress = inst.address;
        VkAccelerationStructureBuildGeometryInfoKHR ti{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        ti.geometryCount = 1; ti.pGeometries = &tg;
        const uint32_t one = 1;
        VkAccelerationStructureBuildSizesInfoKHR ts{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &one, &ts);
        GpuBuffer& tlasBuf = mk(ts.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, local, true);
        GpuBuffer& tlasScr = mk(ts.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, local, true);
        aci.buffer = tlasBuf.buffer; aci.offset = 0; aci.size = ts.accelerationStructureSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (rt.vkCreateAccelerationStructureKHR(dev, &aci, nullptr, &tlas) != VK_SUCCESS) { detail = "TLAS create failed"; break; }
        ti.dstAccelerationStructure = tlas;
        ti.scratchData.deviceAddress = (tlasScr.address + 255) & ~VkDeviceAddress(255);

        // ---- probe points: per (triangle k, micro c) lane; x < 0 = no such micro-triangle ----
        GpuBuffer& pts = mk((VkDeviceSize)N * maxMicro * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, false);
        for (uint32_t k = 0; k < N; ++k)
            for (uint32_t c = 0; c < maxMicro; ++c)
            {
                float* p = static_cast<float*>(pts.mapped) + ((size_t)k * maxMicro + c) * 2;
                if (c >= (1u << (2u * levels[k]))) { p[0] = -1.f; p[1] = -1.f; continue; }
                float uv[3][2];
                ommIndexToBary(c, levels[k], uv);
                p[0] = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.f;
                p[1] = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.f;
            }
        GpuBuffer& hits = mk((VkDeviceSize)N * maxMicro * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, false);
        std::memset(hits.mapped, 0xFF, (size_t)N * maxMicro * 4);

        // ---- compute pipeline ----
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 3; dli.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl) != VK_SUCCESS) { detail = "set layout failed"; break; }
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        if (vkdCreatePipelineLayout(dev, &pli, &pl, "omm.selfTest") != VK_SUCCESS) { detail = "pipeline layout failed"; break; }
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof(kWorldRtOmmTestSpv); smi.pCode = kWorldRtOmmTestSpv;
        VkShaderModule sm = VK_NULL_HANDLE;
        if (vkCreateShaderModule(dev, &smi, nullptr, &sm) != VK_SUCCESS) { detail = "shader module failed"; break; }
        pipe = createComputePipeline(dev, sm, pl);
        vkDestroyShaderModule(dev, sm, nullptr);
        if (!pipe) { detail = "pipeline failed"; break; }
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &pool) != VK_SUCCESS) { detail = "descriptor pool failed"; break; }
        VkDescriptorSetAllocateInfo dai2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai2.descriptorPool = pool; dai2.descriptorSetCount = 1; dai2.pSetLayouts = &dsl;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev, &dai2, &set) != VK_SUCCESS) { detail = "descriptor set failed"; break; }
        VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &tlas;
        VkDescriptorBufferInfo bpts{pts.buffer, 0, VK_WHOLE_SIZE}, bhit{hits.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = i;
            w[i].descriptorCount = 1; w[i].descriptorType = b[i].descriptorType;
        }
        w[0].pNext = &wa; w[1].pBufferInfo = &bpts; w[2].pBufferInfo = &bhit;
        vkUpdateDescriptorSets(dev, 3, w, 0, nullptr);

        ctx.submitImmediate([&](VkCommandBuffer cmd) {
            rt.vkCmdBuildMicromapsEXT(cmd, 1, &mb);
            VkMemoryBarrier2 m1{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m1.srcStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT; m1.srcAccessMask = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
            m1.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR; m1.dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
            VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d1.memoryBarrierCount = 1; d1.pMemoryBarriers = &m1;
            vkCmdPipelineBarrier2(cmd, &d1);
            VkAccelerationStructureBuildRangeInfoKHR r{N, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* rp = &r;
            rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &rp);
            VkMemoryBarrier2 m2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            m2.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            m2.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            m2.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            m2.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d2.memoryBarrierCount = 1; d2.pMemoryBarriers = &m2;
            vkCmdPipelineBarrier2(cmd, &d2);
            VkAccelerationStructureBuildRangeInfoKHR r1{1, 0, 0, 0};
            const VkAccelerationStructureBuildRangeInfoKHR* rp1 = &r1;
            rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &ti, &rp1);
            m2.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            vkCmdPipelineBarrier2(cmd, &d2);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, nullptr);
            const uint32_t push[2] = {N, maxMicro};
            VK_PUSH_BUDGET_CHECK(push);
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            vkCmdDispatch(cmd, (N * maxMicro + 63) / 64, 1, 1);
        });

        // ---- verdict: hit exactly on the diagonal, no candidates ----
        const uint32_t* h = static_cast<const uint32_t*>(hits.mapped);
        uint32_t wrong = 0, cands = 0;
        std::ostringstream obs;
        for (uint32_t k = 0; k < N; ++k)
        {
            std::ostringstream row;
            uint32_t rowHits = 0;
            const uint32_t micro = 1u << (2u * levels[k]);
            for (uint32_t c = 0; c < micro; ++c)
            {
                const uint32_t x = h[k * maxMicro + c];
                if (x & 2u) ++cands;
                const bool hit = (x & 1u) != 0u;
                if (hit != (c == k)) ++wrong;
                if (hit) { row << (rowHits ? "," : "") << c; ++rowHits; }
            }
            if (k < 16) obs << " " << k << "->{" << row.str() << "}";
        }
        ok = wrong == 0 && cands == 0;
        std::ostringstream os;
        os << (prod ? "production shape" : "level " + std::to_string(levels[0])) << ": " << (ok ? "ok" : "MISMATCH")
           << " (" << wrong << " wrong, " << cands << " candidates)";
        if (!ok) os << " observed index->hit points:" << obs.str();
        detail = os.str();
    } while (false);

    if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
    if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    if (tlas) rt.vkDestroyAccelerationStructureKHR(dev, tlas, nullptr);
    if (blas) rt.vkDestroyAccelerationStructureKHR(dev, blas, nullptr);
    if (mm) rt.vkDestroyMicromapEXT(dev, mm, nullptr);
    for (GpuBuffer& bb : bufs) ctx.destroyBuffer(bb);
    return ok;
}
} // namespace

bool ommSelfTest(VkDeviceContext& ctx)
{
    if (!ctx.opacityMicromapEnabled()) return false;
    bool ok = true;
    std::ostringstream os;
    for (uint32_t level = 1; level <= 3 && ok; ++level)
    {
        std::string d;
        ok = selfTestCase(ctx, std::vector<uint32_t>(1u << (2u * level), level), false, d);
        os << " [" << d << "]";
    }
    if (ok)
    {
        // Production shape: 16 triangles alternating levels 2 / 3 in one 4-state micromap.
        std::vector<uint32_t> lv(16);
        for (uint32_t k = 0; k < 16; ++k) lv[k] = 2u + (k & 1u);
        std::string d;
        ok = selfTestCase(ctx, lv, true, d);
        os << " [" << d << "]";
    }
    if (ok) LL_INFOS("VKRT") << "RT: opacity micromap self-test passed" << os.str() << LL_ENDL;
    else    LL_WARNS("VKRT") << "RT: opacity micromap self-test FAILED -- micromaps disabled:" << os.str() << LL_ENDL;
    return ok;
}

// ===========================================================================
// LLCleanRTVK opacity-micromap manager (#194). Declared in llcleanrtvk.h.
//
// PER FRAME (recordBuild):
//   ommPlanFrame  CPU, after readFeedback(): LRU-retire unused entries; rewrite the
//                 per-instance table (only LIVE entries are referenced, so nothing this
//                 frame can point at a BLAS that isn't built); plan new entries for
//                 (slot, alpha key) pairs in use -- micromap + BLAS created up front,
//                 under a per-frame build / micro-triangle budget.
//   ommRecordUploads   staged table rows -> omm_inst / omm_addr (before pass A).
//   ommRecordBuilds    bake (compute) -> vkCmdBuildMicromapsEXT -> OMM BLAS build; the
//                 entries go LIVE here and are referenced from the NEXT frame's table.
// An entry dies by LRU (unused for kOmmKeepFrames), with its slot (slotChanged), or with
// RT itself (releaseAll / shutdown); its micromap + BLAS retire through the frame ring.
// ===========================================================================
#include "llcleanrtvk.h"
#include "llcleanrenderervk.h"
#include "../llvkhitch.h"
#include "../llcleanqvertex.h"   // #573 quantized static vertices
#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#endif

#include <chrono>
#include <cmath>
#include <unordered_set>

namespace
{
constexpr uint32_t kOmmKeepFrames = 300;
constexpr uint32_t kOmmQuarantine = 128;          // a freed OMM id waits this long before reuse (> a full row rotation)
constexpr uint32_t kOmmSliceDiv = 48;             // every instance row refreshed at least every 48 frames
                                                  // (16 with a 4096-row floor was ~0.25 ms/frame of
                                                  // hash lookups in a dense sim for no new builds)
constexpr uint32_t kOmmMaxBuilds = 16;            // entries per frame
constexpr uint64_t kOmmMaxMicro = 4ull << 20;     // micro-triangles baked per frame
constexpr uint64_t kOmmEntryMicro = 1ull << 20;   // per entry: levels drop mesh-wide until it fits
                                                  // (an uncapped 50k-tri mesh at level 6 baked for
                                                  // seconds: GPU context-switch timeout, Xid 109)
constexpr float    kOmmTexelsPerMicro = 16.f;     // target footprint of one micro-triangle
constexpr VkDeviceSize kOmmAlign = 256;
constexpr size_t kInstBytesO = 224, kSlotBytesO = 32, kMatBytes = 96;
constexpr uint32_t kSentinelO = 0xFFFFFFFFu;

inline VkDeviceSize alignO(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }
struct SlotInfoCPU { uint32_t first_index, index_count; int32_t base_vertex; uint32_t has_weights, vert_count, p0, p1, p2; };
inline uint32_t rdU(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline float rdF(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }
inline uint32_t microOf(uint32_t level) { return 1u << (2u * level); }
inline uint32_t bytesOf(uint32_t level) { return std::max(1u, microOf(level) / 4u); }   // 4-state: 2 bits each

void barrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
{
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

uint64_t hashKey(const uint32_t* w, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= w[i]; h *= 1099511628211ull; }
    return h;
}

bool ommSettingOn()
{
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTOpacityMicromaps", true);
    return s_on;
#else
    return true;
#endif
}

uint32_t ommMaxLevelSetting(uint32_t dev_max)
{
    uint32_t lvl = 6;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_lvl(gSavedSettings, "RenderVKRTOmmMaxLevel", 6);
    lvl = s_lvl;
#endif
    return std::min<uint32_t>(std::min<uint32_t>(lvl, dev_max), 9u);
}
} // namespace

bool LLCleanRTVK::OmmKey::operator==(const OmmKey& o) const
{
    return slot == o.slot && slotGen == o.slotGen && mat == o.mat && tex == o.tex && texVer == o.texVer
        && tintA == o.tintA && std::memcmp(uv, o.uv, sizeof(uv)) == 0;
}

// One planned entry build (this frame).
struct LLCleanRTVK::OmmBuild
{
    uint32_t id = 0, inst = 0, prims = 0;
    uint32_t first_index = 0, vert_count = 0;
    int32_t  base_vertex = 0;
    VkDeviceSize dataOff = 0, dataBytes = 0;          // in mOmmData[slot]
    VkDeviceSize recOff = 0, triOff = 0, idxOff = 0;  // in mOmmUp[slot]
    VkDeviceSize xformOff = 0;                        // in mOmmUp[slot]: the decode transform (#573)
    float xf[12] = {};                                // VkTransformMatrixKHR rows (quantized -> object)
    VkDeviceSize mmScratch = 0, asScratch = 0;
    std::vector<uint8_t> level;                       // per triangle
    VkMicromapUsageEXT usage[13] = {};
    uint32_t nUsage = 0;
};

bool LLCleanRTVK::ommInit()
{
    if (!mOmmOk) return false;
    VkDevice dev = mCtx->device();
    VkDescriptorSetLayoutBinding b[6]{};
    for (uint32_t i = 0; i < 6; ++i)
    {
        b[i].binding = i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 6; ci.pBindings = b;
    if (vkCreateDescriptorSetLayout(dev, &ci, nullptr, &mDslOmmBake) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout sl[2] = {mDslOmmBake, mR.mDslDraw1};
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2; li.pSetLayouts = sl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
    if (vkdCreatePipelineLayout(dev, &li, &mOmmBakeLayout, "mOmmBakeLayout") != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(kWorldRtOmmBakeSpv); smi.pCode = kWorldRtOmmBakeSpv;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &sm) != VK_SUCCESS) return false;
    mOmmBakePipe = createComputePipeline(dev, sm, mOmmBakeLayout);
    vkDestroyShaderModule(dev, sm, nullptr);
    return mOmmBakePipe != VK_NULL_HANDLE;
}

void LLCleanRTVK::ommShutdown()
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (OmmEntry& e : mOmm)
    {
        if (e.as) mCtx->rt().vkDestroyAccelerationStructureKHR(dev, e.as, nullptr);
        if (e.mm) mCtx->rt().vkDestroyMicromapEXT(dev, e.mm, nullptr);
        if (e.mmBuf.buffer) mCtx->destroyBuffer(e.mmBuf);
    }
    mOmm.clear(); mOmmFree.clear(); mOmmByHash.clear(); mOmmBySlot.clear();
    mOmmAddrCpu.clear(); mOmmInstCpu.clear();
    for (GpuBuffer* b : {&mOmmAddr, &mOmmInst, &mOmmUp[0], &mOmmUp[1], &mOmmUp[2],
                         &mOmmData[0], &mOmmData[1], &mOmmData[2]})
        if (b->buffer) mCtx->destroyBuffer(*b);
    if (mOmmBakePipe) vkDestroyPipeline(dev, mOmmBakePipe, nullptr);
    if (mOmmBakeLayout) vkDestroyPipelineLayout(dev, mOmmBakeLayout, nullptr);
    if (mDslOmmBake) vkDestroyDescriptorSetLayout(dev, mDslOmmBake, nullptr);
    mOmmBakePipe = VK_NULL_HANDLE; mOmmBakeLayout = VK_NULL_HANDLE; mDslOmmBake = VK_NULL_HANDLE;
    delete mOmmPlan; mOmmPlan = nullptr;
    mOmmBytes = 0;
}

void LLCleanRTVK::ommRetire(uint32_t id, uint32_t why)
{
    if (id >= mOmm.size()) return;
    OmmEntry& e = mOmm[id];
    if (!e.as && !e.mm) return;
    if (why < 6) ++mOmmWhy[why];
    if (e.as) retireAS(e.as, e.asMem);
    else if (e.asMem.chunk >= 0) mPend.allocs.push_back(e.asMem);
    if (e.mm)
    {
        mPend.micromaps.push_back(e.mm);
        if (e.mmBuf.buffer) mPend.bufs.push_back(e.mmBuf);
        else mPend.allocs.push_back(e.mmMem);
    }
    mOmmBytes -= std::min<uint64_t>(mOmmBytes, e.asMem.size + e.mmMem.size);
    // Unindex.
    const uint64_t h = hashKey(reinterpret_cast<const uint32_t*>(&e.key), sizeof(OmmKey) / 4);
    auto range = mOmmByHash.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second == id) { mOmmByHash.erase(it); break; }
    auto bs = mOmmBySlot.find(e.key.slot);
    if (bs != mOmmBySlot.end())
    {
        auto& v = bs->second;
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
        if (v.empty()) mOmmBySlot.erase(bs);
    }
    if (e.live && mOmmLive) --mOmmLive;
    ++mOmmRetired;
    e = OmmEntry{};
    mOmmFree.emplace_back(id, mFrame);
    if ((size_t)id * 4 + 3 < mOmmAddrCpu.size())
    {
        std::fill(mOmmAddrCpu.begin() + (size_t)id * 4, mOmmAddrCpu.begin() + (size_t)id * 4 + 4, 0u);
        mOmmAddrDirtyLo = std::min(mOmmAddrDirtyLo, id);
        mOmmAddrDirtyHi = std::max(mOmmAddrDirtyHi, id + 1);
    }
}

void LLCleanRTVK::ommSlotChanged(uint32_t sl)
{
    auto it = mOmmBySlot.find(sl);
    if (it == mOmmBySlot.end()) return;
    const std::vector<uint32_t> ids = it->second;   // ommRetire edits the index
    for (uint32_t id : ids) ommRetire(id);
}

void LLCleanRTVK::ommReleaseAll()
{
    for (uint32_t id = 0; id < (uint32_t)mOmm.size(); ++id) ommRetire(id, 1);
    std::fill(mOmmInstCpu.begin(), mOmmInstCpu.end(), 0u);
    if (!mOmmInstCpu.empty()) { mOmmInstDirtyLo = 0; mOmmInstDirtyHi = (uint32_t)(mOmmInstCpu.size() / 4); }
}

VkDeviceSize LLCleanRTVK::ommPlanFrame(const float lod_cam[3], float radius, VkDeviceSize salign, uint64_t budget)
{
    VkMemScope memcat(VKMEM_RT_AUX);   // #407 (micromap storage is RT_AS regardless)
    if (!mOmmPlan) mOmmPlan = new std::vector<OmmBuild>();
    std::vector<OmmBuild>& plan = *mOmmPlan;
    plan.clear();
    mOmmBuilt = 0; mOmmUsedInst = 0;
    mOmmTablesOn = false;
    if (!mOmmOk || !mOmmBakePipe) return 0;
    LLCleanRendererVK& R = mR;
    const uint32_t inst_n = std::min<uint32_t>(R.mInstanceCount, (uint32_t)(R.mCpuInstances.size() / kInstBytesO));
    if (mOmmInstCpu.size() < (size_t)inst_n * 4)
    {
        const uint32_t old = (uint32_t)(mOmmInstCpu.size() / 4);
        mOmmInstCpu.resize((size_t)inst_n * 4, 0u);
        mOmmInstDirtyLo = std::min(mOmmInstDirtyLo, old);
        mOmmInstDirtyHi = std::max(mOmmInstDirtyHi, inst_n);
    }
    if (!ommSettingOn())
    {
        // Off: pass A binds the zero dummy (no OMM references); entries retire.
        if (mOmmLive) ommReleaseAll();
        return 0;
    }
    VK_HITCH_SCOPE("vk_rtOmmPlan");
    {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        static LLCachedControl<U32> s_dbgStage(gSavedSettings, "RenderVKRTOmmDebugStage", 0);
        if ((U32)s_dbgStage != 0 && now > mOmmNextLog)
        {
            mOmmNextLog = now + 2.0;
            LL_INFOS("VKRT") << "OMM 2s: planned " << mOmmPlanned << " recorded " << mOmmRecorded << " live " << mOmmLive
                             << " entries " << mOmm.size() << " free " << mOmmFree.size() << " tablesOff " << mOmmTablesOff
                             << " | retired: slotChanged " << mOmmWhy[0] << " releaseAll " << mOmmWhy[1] << " LRU " << mOmmWhy[2]
                             << " noTables " << mOmmWhy[3] << " abort " << mOmmWhy[4] << " discard " << mOmmWhy[5]
                             << " | tablesOn " << mOmmTablesOn << " up " << (mOmmUp[mSlotIdx].mapped != nullptr)
                             << " inst rows " << mOmmUsedInst << LL_ENDL;
            mOmmPlanned = mOmmRecorded = mOmmTablesOff = 0;
            for (uint32_t& w : mOmmWhy) w = 0;
        }
    }

    const VkDevice dev = mCtx->device();
    const auto& rt = mCtx->rt();

    // ---- LRU ----
    for (uint32_t id = 0; id < (uint32_t)mOmm.size(); ++id)
        if (mOmm[id].live && mOmm[id].lastUsed + kOmmKeepFrames < mFrame) ommRetire(id, 2);

    const uint8_t* insts = R.mCpuInstances.data();
    const uint8_t* mats = R.mCpuMaterials.data();
    const uint32_t mat_n = (uint32_t)(R.mCpuMaterials.size() / kMatBytes);
    const uint8_t* groups = R.mCpuGroups.data();
    const uint32_t grp_n = (uint32_t)(R.mCpuGroups.size() / 16);
    const uint8_t* slots = R.mCpuSlots.data();
    const uint32_t slot_n = std::min<uint32_t>((uint32_t)(R.mCpuSlots.size() / kSlotBytesO), (uint32_t)mSlot.size());
    const uint32_t maxL = ommMaxLevelSetting(mCtx->ommMaxLevel4State());
    const float r2 = radius;

    uint32_t dbgRowBits = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    {
        static LLCachedControl<U32> s_stage(gSavedSettings, "RenderVKRTOmmDebugStage", 0);
        if ((U32)s_stage == 4) dbgRowBits = 0x40000000u;
        if ((U32)s_stage == 5) dbgRowBits = 0x20000000u;
        if ((U32)s_stage == 6) dbgRowBits = 0xFFFFFFFFu;   // live + accumulating, never referenced
    }
#endif
    uint32_t builds = 0;
    uint64_t micro_total = 0;
    VkDeviceSize up_bytes = 0, data_bytes = 0, scratch = 0;
    const uint64_t omm_cap = budget / 4;   // micromaps + their BLAS: at most a quarter of RT memory

    // Rows to (re)resolve this frame: changed instances + a round-robin slice (every row
    // at least every kOmmSliceDiv frames). Rows not visited keep their value: a retired
    // entry's address row is zeroed (pass A falls back to the plain BLAS), freed ids are
    // quarantined past a full rotation, and pass A checks the entry's slot.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    if (mOmmDirtyAll) { ranges.emplace_back(0u, inst_n); mOmmDirtyAll = false; }
    else
    {
        for (const auto& r : mOmmDirtyRuns) ranges.emplace_back(std::min(r.first, inst_n), std::min(r.second, inst_n));
        const uint32_t slice = std::max<uint32_t>(1024u, inst_n / kOmmSliceDiv + 1);
        if (mOmmCursor >= inst_n) mOmmCursor = 0;
        const uint32_t end = std::min(inst_n, mOmmCursor + slice);
        ranges.emplace_back(mOmmCursor, end);
        if (end - mOmmCursor < slice) ranges.emplace_back(0u, std::min(inst_n, slice - (end - mOmmCursor)));
        mOmmCursor = end >= inst_n ? 0u : end;
    }
    mOmmDirtyRuns.clear();
    for (const auto& range : ranges)
    for (uint32_t id = range.first; id < range.second; ++id)
    {
        const uint8_t* I = insts + (size_t)id * kInstBytesO;
        uint32_t row[4] = {0, 0, 0, 0};
        const uint32_t f = rdU(I + 184);
        const uint32_t gid = rdU(I + 176);
        const uint32_t mid = rdU(I + 180);
        // Static / worn-rigid alpha-MASK instances whose UVs are a fixed affine map of the
        // vertex UVs (no planar texgen, no texture animation). Rigged, blend, terrain,
        // water, HUD and exclusion surfaces keep the plain path.
        const bool cand = (f & 4u) && !(f & (1u | 2u | 8u | 32u | 64u | 1024u | 2048u | 65536u))
                       && gid < grp_n && mid < mat_n;
        if (cand)
        {
            const uint8_t* M = mats + (size_t)mid * kMatBytes;
            const uint32_t tex = rdU(M + 0);
            const float cutoff = rdF(M + 88);
            const uint32_t mflags = rdU(M + 92);
            // #568: the mirror row is store frame (agent + R.mInstD); lod_cam is agent space.
            const float tx = rdF(I + 48) - R.mInstD[0], ty = rdF(I + 52) - R.mInstD[1], tz = rdF(I + 56) - R.mInstD[2];
            const float dx = tx - lod_cam[0], dy = ty - lod_cam[1], dz = tz - lod_cam[2];
            const float reach = r2 + std::max(rdF(I + 76), 0.f);
            const bool in_reach = r2 <= 0.f || dx * dx + dy * dy + dz * dz <= reach * reach;
            if ((mflags & 1u) && cutoff >= 0.f && in_reach && tex < R.mTextures.size()
                && R.mTextures[tex].extent.width >= 32 && R.mTextures[tex].extent.height >= 32)
            {
                OmmKey key;
                key.mat = mid; key.tex = tex;
                key.texVer = tex < R.mTexContentVer.size() ? R.mTexContentVer[tex] : 0u;   // (#614: not bumped by a pool move)
                key.tintA = rdU(I + 108);
                key.uv[0] = rdF(I + 112); key.uv[1] = rdF(I + 116);   // uv_matrix col 0 .xy
                key.uv[2] = rdF(I + 128); key.uv[3] = rdF(I + 132);   // col 1 .xy
                key.uv[4] = rdF(I + 160); key.uv[5] = rdF(I + 164);   // col 3 .xy (translation)
                for (uint32_t L = 0; L < 4; ++L)
                {
                    const uint32_t s = rdU(groups + (size_t)gid * 16 + L * 4);
                    if (s == kSentinelO || s >= slot_n) continue;
                    // Same lifecycle as the slot's own BLAS: built, and selected recently.
                    if (!mSlot[s].as || mSlot[s].lastUsed + 2 < mFrame) continue;
                    // ... and STABLE: a slot whose content keeps changing (re-placed every
                    // frame) would retire + rebake its micromap every frame for nothing.
                    if (s < mSlotChangedAt.size() && mSlotChangedAt[s] + 120 > mFrame) continue;
                    key.slot = s;
                    key.slotGen = s < mSlotGen.size() ? mSlotGen[s] : 0u;
                    const uint64_t h = hashKey(reinterpret_cast<const uint32_t*>(&key), sizeof(OmmKey) / 4);
                    uint32_t found = kSentinelO;
                    auto range = mOmmByHash.equal_range(h);
                    for (auto it = range.first; it != range.second; ++it)
                        if (mOmm[it->second].key == key) { found = it->second; break; }
                    if (found != kSentinelO)
                    {
                        OmmEntry& e = mOmm[found];
                        e.lastUsed = mFrame;
                        if (e.live && dbgRowBits != 0xFFFFFFFFu) row[L] = (found + 1u) | dbgRowBits;
                        continue;
                    }
                    if (builds >= kOmmMaxBuilds || micro_total >= kOmmMaxMicro || mOmmBytes >= omm_cap) continue;
                    // ---- plan a new entry ----
                    const SlotInfoCPU* si = reinterpret_cast<const SlotInfoCPU*>(slots + (size_t)s * kSlotBytesO);
                    // #584: the slot's bytes from the mesh store mirror (header-prefixed
                    // vertices, face-local indices) -- no CPU image of the packed buffers.
                    const uint8_t* sverts = nullptr; const uint16_t* sidx = nullptr; const uint8_t* swgt = nullptr;
                    LLCleanQV::QHeader qh;
                    if (si->index_count < 3 || si->base_vertex < 0
                        || !R.geomSlotSource(s, sverts, sidx, swgt)
                        || !LLCleanQV::headerAt(sverts, (size_t)si->vert_count + LLCleanQV::kHeaderVerts,
                                                LLCleanQV::kHeaderVerts, qh))
                        continue;
                    OmmBuild b;
                    LLCleanQV::snormTransform(qh, b.xf);
                    b.inst = id; b.prims = si->index_count / 3;
                    b.first_index = si->first_index; b.base_vertex = si->base_vertex; b.vert_count = si->vert_count;
                    b.level.resize(b.prims);
                    const float tw = (float)R.mTextures[tex].extent.width, th = (float)R.mTextures[tex].extent.height;
                    uint32_t perLevel[13] = {};
                    uint64_t micro = 0;
                    std::vector<uint8_t>& want = b.level;   // ideal level per triangle, then capped
                    for (uint32_t p = 0; p < b.prims; ++p)
                    {
                        float uv[3][2];
                        for (int k = 0; k < 3; ++k)
                        {
                            const uint32_t vi = sidx[p * 3 + k];
                            LLCleanQV::QVertex qv;
                            std::memcpy(&qv, sverts + ((size_t)LLCleanQV::kHeaderVerts + std::min(vi, si->vert_count - 1))
                                                      * LLCleanQV::kStride, sizeof(qv));
                            float quv[2];
                            LLCleanQV::decodeUV(qh, qv, quv);
                            const float u = quv[0], v = quv[1];
                            uv[k][0] = key.uv[0] * u + key.uv[2] * v + key.uv[4];
                            uv[k][1] = key.uv[1] * u + key.uv[3] * v + key.uv[5];
                        }
                        const float ex = (uv[1][0] - uv[0][0]) * tw, ey = (uv[1][1] - uv[0][1]) * th;
                        const float fx = (uv[2][0] - uv[0][0]) * tw, fy = (uv[2][1] - uv[0][1]) * th;
                        const float area = 0.5f * std::fabs(ex * fy - ey * fx);   // texels
                        uint32_t L2 = 0;
                        if (area > kOmmTexelsPerMicro && std::isfinite(area))
                            L2 = (uint32_t)std::ceil(0.5 * std::log2(area / kOmmTexelsPerMicro));
                        want[p] = (uint8_t)std::min(L2, maxL);
                    }
                    // Fit the entry: lower the cap until the mesh's micro-triangles fit.
                    uint32_t cap = maxL;
                    for (;;)
                    {
                        micro = 0;
                        for (uint32_t p = 0; p < b.prims; ++p) micro += microOf(std::min<uint32_t>(want[p], cap));
                        if (micro <= kOmmEntryMicro || cap == 0) break;
                        --cap;
                    }
                    if (micro > kOmmEntryMicro || micro_total + micro > kOmmMaxMicro) continue;   // too big / next frame
                    for (uint32_t p = 0; p < b.prims; ++p)
                    {
                        const uint32_t L2 = std::min<uint32_t>(want[p], cap);
                        b.level[p] = (uint8_t)L2;
                        ++perLevel[L2];
                        b.dataBytes += bytesOf(L2);
                    }
                    for (uint32_t l = 0; l <= maxL && l < 13; ++l)
                        if (perLevel[l]) b.usage[b.nUsage++] = VkMicromapUsageEXT{perLevel[l], l, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT};
                    // Sizes: micromap, then the OMM BLAS.
                    VkMicromapBuildInfoEXT mb{VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT};
                    mb.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
                    mb.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
                    mb.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
                    mb.usageCountsCount = b.nUsage; mb.pUsageCounts = b.usage;
                    VkMicromapBuildSizesInfoEXT ms{VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT};
                    rt.vkGetMicromapBuildSizesEXT(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &mb, &ms);
                    VkAccelerationStructureTrianglesOpacityMicromapEXT om{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT};
                    om.indexType = VK_INDEX_TYPE_UINT32; om.indexStride = 4;
                    om.usageCountsCount = b.nUsage; om.pUsageCounts = b.usage;
                    VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                    auto& t = g.geometry.triangles;
                    t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                    t.pNext = &om;
                    t.vertexFormat = VK_FORMAT_R16G16B16A16_SNORM; t.vertexStride = LLCleanQV::kStride;   // (#573)
                    t.maxVertex = si->vert_count - 1; t.indexType = VK_INDEX_TYPE_UINT16;
                    t.transformData.deviceAddress = 16;   // sizes: non-null like the build's (#573)
                    VkAccelerationStructureBuildGeometryInfoKHR gi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                    gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                    gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT;
                    gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
                    gi.geometryCount = 1; gi.pGeometries = &g;
                    VkAccelerationStructureBuildSizesInfoKHR as{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                    rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, &b.prims, &as);
                    if (!ms.micromapSize || !as.accelerationStructureSize) continue;
                    if (mStaticBytes + mRigBytes + mOmmBytes + ms.micromapSize + as.accelerationStructureSize > budget) continue;
                    bool dedicated = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
                    static LLCachedControl<bool> s_ded(gSavedSettings, "RenderVKRTOmmDedicated", false);
                    dedicated = s_ded;
#endif
                    Alloc mmMem, asMem;
                    GpuBuffer mmBuf;
                    if (dedicated)
                    {
                        mmBuf = mCtx->createBuffer(ms.micromapSize, VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
                        if (!mmBuf.buffer) continue;
                    }
                    else if (!arenaAlloc(ms.micromapSize, mmMem)) continue;
                    auto dropMm = [&]() { if (mmBuf.buffer) mCtx->destroyBuffer(mmBuf); else arenaFree(mmMem); };
                    VkMicromapCreateInfoEXT mci{VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT};
                    mci.buffer = mmBuf.buffer ? mmBuf.buffer : mChunks[mmMem.chunk].buf.buffer;
                    mci.offset = mmBuf.buffer ? 0 : mmMem.off; mci.size = ms.micromapSize;
                    mci.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
                    VkMicromapEXT mmH = VK_NULL_HANDLE;
                    if (rt.vkCreateMicromapEXT(dev, &mci, nullptr, &mmH) != VK_SUCCESS) { dropMm(); continue; }
                    // BLAS size WITH the micromap attached (the query without one may come
                    // back smaller; a build overflowing its arena allocation corrupts the
                    // neighbouring BLAS -- random-address faults, seconds later). + margin.
                    om.micromap = mmH;
                    VkAccelerationStructureBuildSizesInfoKHR as2{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                    rt.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &gi, &b.prims, &as2);
                    {
                        static uint32_t s_sizeLog = 0;
                        if (as2.accelerationStructureSize != as.accelerationStructureSize && s_sizeLog++ < 10)
                            LL_INFOS("VKRT") << "OMM BLAS size: " << as.accelerationStructureSize << " without micromap, "
                                             << as2.accelerationStructureSize << " with" << LL_ENDL;
                    }
                    as.accelerationStructureSize = std::max(as.accelerationStructureSize, as2.accelerationStructureSize);
                    as.buildScratchSize = std::max(as.buildScratchSize, as2.buildScratchSize);
                    if (!arenaAlloc(as.accelerationStructureSize + as.accelerationStructureSize / 4 + 256, asMem))
                    { rt.vkDestroyMicromapEXT(dev, mmH, nullptr); dropMm(); continue; }
                    VkDeviceAddress addr = 0;
                    VkAccelerationStructureKHR asH = createAS(asMem, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, addr, "blas-omm", s);
                    if (!asH) { rt.vkDestroyMicromapEXT(dev, mmH, nullptr); dropMm(); arenaFree(asMem); continue; }
                    uint32_t eid;
                    if (!mOmmFree.empty() && mOmmFree.front().second + kOmmQuarantine < mFrame)
                    { eid = mOmmFree.front().first; mOmmFree.pop_front(); }
                    else { eid = (uint32_t)mOmm.size(); mOmm.emplace_back(); }
                    OmmEntry& e = mOmm[eid];
                    e = OmmEntry{};
                    e.key = key; e.mm = mmH; e.mmMem = mmMem; e.as = asH; e.asMem = asMem; e.addr = addr;
                    e.mmBuf = mmBuf;
                    e.lastUsed = mFrame; e.live = false;
                    mOmmByHash.emplace(h, eid);
                    mOmmBySlot[s].push_back(eid);
                    mOmmBytes += mmMem.size + asMem.size;
                    b.id = eid;
                    b.mmScratch = ms.buildScratchSize; b.asScratch = as.buildScratchSize;
                    b.dataOff = data_bytes; data_bytes = alignO(data_bytes + b.dataBytes, kOmmAlign);
                    b.recOff = up_bytes;   up_bytes = alignO(up_bytes + (VkDeviceSize)b.prims * 32, kOmmAlign);
                    b.triOff = up_bytes;   up_bytes = alignO(up_bytes + (VkDeviceSize)b.prims * 8, kOmmAlign);
                    b.idxOff = up_bytes;   up_bytes = alignO(up_bytes + (VkDeviceSize)b.prims * 4, kOmmAlign);
                    b.xformOff = up_bytes; up_bytes = alignO(up_bytes + sizeof(VkTransformMatrixKHR), kOmmAlign);
                    scratch += alignO(b.mmScratch + 256, salign) + alignO(b.asScratch, salign);
                    micro_total += micro;
                    ++builds;
                    plan.push_back(std::move(b));
                }
            }
        }
        uint32_t* cur = &mOmmInstCpu[(size_t)id * 4];
        if (row[0] | row[1] | row[2] | row[3]) ++mOmmUsedInst;
        if (std::memcmp(cur, row, sizeof(row)) != 0)
        {
            std::memcpy(cur, row, sizeof(row));
            mOmmInstDirtyLo = std::min(mOmmInstDirtyLo, id);
            mOmmInstDirtyHi = std::max(mOmmInstDirtyHi, id + 1);
        }
    }
    if (mOmmAddrCpu.size() < mOmm.size() * 4)
    {
        const uint32_t old = (uint32_t)(mOmmAddrCpu.size() / 4);
        mOmmAddrCpu.resize(mOmm.size() * 4, 0u);
        mOmmAddrDirtyLo = std::min(mOmmAddrDirtyLo, old);
        mOmmAddrDirtyHi = std::max(mOmmAddrDirtyHi, (uint32_t)mOmm.size());
    }
    // Transient inputs for this frame slot (its previous user is done).
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize inst_rows = (VkDeviceSize)(mOmmInstDirtyLo < mOmmInstDirtyHi ? mOmmInstDirtyHi - mOmmInstDirtyLo : 0) * 16;
    const VkDeviceSize addr_rows = (VkDeviceSize)(mOmmAddrDirtyLo < mOmmAddrDirtyHi ? mOmmAddrDirtyHi - mOmmAddrDirtyLo : 0) * 16;
    const VkDeviceSize up_need = up_bytes + inst_rows + addr_rows + 64;
    mOmmUpRowsAt = up_bytes;
    const VkBufferUsageFlags upUse = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    GpuBuffer& up = mOmmUp[mSlotIdx];
    if (up.size < up_need || !up.mapped)
    {
        retireBuf(up);
        up = mCtx->createBuffer(alignO(up_need + up_need / 2, 1u << 16), upUse, host, true);
    }
    // Tables (grow = fresh buffer: zero-filled, every row re-sent).
    const VkBufferUsageFlags tu = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const uint32_t omm_n = (uint32_t)(mOmmAddrCpu.size() / 4);
    if (mOmmInst.size < (VkDeviceSize)std::max(inst_n, 1u) * 16 || mOmmAddr.size < (VkDeviceSize)std::max(omm_n, 1u) * 16)
    {
        retireBuf(mOmmInst); retireBuf(mOmmAddr);
        mOmmInst = mCtx->createBuffer((VkDeviceSize)std::max(inst_n + inst_n / 4, 1024u) * 16, tu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mOmmAddr = mCtx->createBuffer((VkDeviceSize)std::max(omm_n + omm_n / 2, 256u) * 16, tu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        mOmmTableFill = true;
        mOmmInstDirtyLo = 0; mOmmInstDirtyHi = inst_n;
        mOmmAddrDirtyLo = 0; mOmmAddrDirtyHi = omm_n;
        // Rows to stage grew: resize the upload need.
        const VkDeviceSize need2 = up_bytes + (VkDeviceSize)inst_n * 16 + (VkDeviceSize)omm_n * 16 + 64;
        if (up.size < need2 || !up.mapped)
        {
            retireBuf(up);
            up = mCtx->createBuffer(alignO(need2 + need2 / 2, 1u << 16), upUse, host, true);
        }
    }
    mOmmTablesOn = up.mapped && mOmmInst.buffer && mOmmAddr.buffer;
    if (!plan.empty())
    {
        GpuBuffer& db = mOmmData[mSlotIdx];
        if (db.size < data_bytes)
        {
            retireBuf(db);
            db = mCtx->createBuffer(alignO(data_bytes + data_bytes / 2, 1u << 16), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                    | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT
                                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        }
        if (!mOmmTablesOn || !db.buffer)
        {
            for (const OmmBuild& b : plan) ommRetire(b.id, 3);
            ++mOmmTablesOff;
            plan.clear();
            return 0;
        }
        // CPU inputs: bake records, the micromap triangle array, the per-triangle OMM index.
        uint8_t* base = static_cast<uint8_t*>(up.mapped);
        for (const OmmBuild& b : plan)
        {
            uint32_t* rec = reinterpret_cast<uint32_t*>(base + b.recOff);
            auto* tri = reinterpret_cast<VkMicromapTriangleEXT*>(base + b.triOff);
            uint32_t* idx = reinterpret_cast<uint32_t*>(base + b.idxOff);
            std::memcpy(base + b.xformOff, b.xf, sizeof(VkTransformMatrixKHR));   // decode transform (#573)
            uint32_t off = 0;
            for (uint32_t p = 0; p < b.prims; ++p)
            {
                const uint32_t L = b.level[p];
                uint32_t* r = rec + p * 8;
                r[0] = b.inst; r[1] = b.first_index + p * 3; r[2] = (uint32_t)b.base_vertex;
                r[3] = (uint32_t)((b.dataOff + off) * 8); r[4] = L; r[5] = r[6] = r[7] = 0;
                tri[p].dataOffset = off;
                tri[p].subdivisionLevel = (uint16_t)L;
                tri[p].format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;
                idx[p] = p;
                off += bytesOf(L);
            }
        }
    }
    mOmmBuilt = (uint32_t)plan.size();
    mOmmPlanned += (uint32_t)plan.size();
    return scratch;
}

void LLCleanRTVK::ommAbortPlan()
{
    if (!mOmmPlan) return;
    for (const OmmBuild& b : *mOmmPlan) ommRetire(b.id, 4);   // never built, never referenced
    mOmmPlan->clear();
}

void LLCleanRTVK::ommRecordUploads(VkCommandBuffer cmd)
{
    if (!mOmmTablesOn) return;
    GpuBuffer& up = mOmmUp[mSlotIdx];
    if (mOmmTableFill)
    {
        vkCmdFillBuffer(cmd, mOmmInst.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, mOmmAddr.buffer, 0, VK_WHOLE_SIZE, 0);
        barrier2(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        mOmmTableFill = false;
    }
    // Rows are staged after this frame's bake inputs (ommPlanFrame sized the buffer for
    // both and only turned the tables on when everything fits).
    const uint32_t inst_n = (uint32_t)(mOmmInstCpu.size() / 4);
    const uint32_t omm_n = (uint32_t)(mOmmAddrCpu.size() / 4);
    VkDeviceSize at = mOmmUpRowsAt;
    uint8_t* base = static_cast<uint8_t*>(up.mapped);
    if (mOmmInstDirtyLo < mOmmInstDirtyHi)
    {
        const uint32_t hi = std::min(mOmmInstDirtyHi, inst_n);
        const VkDeviceSize n = (VkDeviceSize)(hi - mOmmInstDirtyLo) * 16;
        std::memcpy(base + at, mOmmInstCpu.data() + (size_t)mOmmInstDirtyLo * 4, n);
        VkBufferCopy c{at, (VkDeviceSize)mOmmInstDirtyLo * 16, n};
        vkCmdCopyBuffer(cmd, up.buffer, mOmmInst.buffer, 1, &c);
        at = alignO(at + n, 16);
        mOmmInstDirtyLo = ~0u; mOmmInstDirtyHi = 0;
    }
    if (mOmmAddrDirtyLo < mOmmAddrDirtyHi)
    {
        const uint32_t hi = std::min(mOmmAddrDirtyHi, omm_n);
        const VkDeviceSize n = (VkDeviceSize)(hi - mOmmAddrDirtyLo) * 16;
        std::memcpy(base + at, mOmmAddrCpu.data() + (size_t)mOmmAddrDirtyLo * 4, n);
        VkBufferCopy c{at, (VkDeviceSize)mOmmAddrDirtyLo * 16, n};
        vkCmdCopyBuffer(cmd, up.buffer, mOmmAddr.buffer, 1, &c);
        mOmmAddrDirtyLo = ~0u; mOmmAddrDirtyHi = 0;
    }
}

void LLCleanRTVK::ommRecordBuilds(VkCommandBuffer cmd, const std::function<VkDeviceAddress(VkDeviceSize)>& takeScratch)
{
    if (!mOmmPlan || mOmmPlan->empty()) return;
    std::vector<OmmBuild>& plan = *mOmmPlan;
    const auto& rt = mCtx->rt();
    VkDevice dev = mCtx->device();
    LLCleanRendererVK& R = mR;
    GpuBuffer& up = mOmmUp[mSlotIdx];
    GpuBuffer& db = mOmmData[mSlotIdx];
    // DEBUG (RenderVKRTOmmDebugStage): stop after a stage and discard the entries (never
    // referenced): 1 bake, 2 + micromap build, 3 + OMM BLAS build. 0 = normal.
    uint32_t stop = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_stage(gSavedSettings, "RenderVKRTOmmDebugStage", 0);
    stop = s_stage;
#endif
    auto discard = [&]() {
        for (const OmmBuild& b : plan) ommRetire(b.id, 5);
        plan.clear();
    };

    // A planned entry must still hold its objects (nothing may have retired it since).
    plan.erase(std::remove_if(plan.begin(), plan.end(), [&](const OmmBuild& b) {
        const bool bad = b.id >= mOmm.size() || !mOmm[b.id].mm || !mOmm[b.id].as;
        if (bad) LL_WARNS_ONCE("VKRT") << "OMM: planned entry " << b.id << " lost its objects before recording" << LL_ENDL;
        return bad;
    }), plan.end());
    if (plan.empty()) return;
    // ---- bake: zero the data, one workgroup per micromap triangle ----
    VkDeviceSize data_end = 0;
    uint32_t rec_n = 0;
    for (const OmmBuild& b : plan) { data_end = std::max(data_end, b.dataOff + b.dataBytes); rec_n += b.prims; }
    vkCmdFillBuffer(cmd, db.buffer, 0, alignO(data_end, 4), 0);
    barrier2(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    {
        // Bake set: this frame slot's (rewritten every use; buffers may have been reallocated).
        if (!mSetOmmBake[mSlotIdx])
        {
            VkDescriptorSetLayout l = mDslOmmBake;
            if (mPoolOmm == VK_NULL_HANDLE)
            {
                VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 18};
                VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pi.maxSets = 3; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
                vkCreateDescriptorPool(dev, &pi, nullptr, &mPoolOmm);
            }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = mPoolOmm; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
            if (!mPoolOmm || vkAllocateDescriptorSets(dev, &ai, &mSetOmmBake[mSlotIdx]) != VK_SUCCESS) return;
        }
        // The bake records live in the upload buffer: bind the whole buffer and index by
        // a record base (records of all builds are contiguous? no -- one dispatch per build).
        VkDescriptorBufferInfo bi[6] = {{R.mInstanceSSBO.buffer, 0, VK_WHOLE_SIZE}, {R.mMaterialSSBO.buffer, 0, VK_WHOLE_SIZE},
                                        {R.mVbo.buffer, 0, VK_WHOLE_SIZE}, {R.mIbo.buffer, 0, VK_WHOLE_SIZE},
                                        {up.buffer, 0, VK_WHOLE_SIZE}, {db.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[6]{};
        for (uint32_t i = 0; i < 6; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = mSetOmmBake[mSlotIdx];
            w[i].dstBinding = i; w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(dev, 6, w, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mOmmBakePipe);
        const VkDescriptorSet sets[2] = {mSetOmmBake[mSlotIdx], R.mDsetDraw1};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mOmmBakeLayout, 0, 2, sets, 0, nullptr);
        for (const OmmBuild& b : plan)
        {
            // Records are 32 B: base = recOff / 32 (recOff is 256-aligned).
            const uint32_t push[2] = {(uint32_t)(b.recOff / 32) + b.prims, (uint32_t)(b.recOff / 32)};
            VK_PUSH_BUDGET_CHECK(push);
            vkCmdPushConstants(cmd, mOmmBakeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            const uint32_t gx = std::min(b.prims, 65535u), gy = (b.prims + 65534u) / 65535u;
            vkCmdDispatch(cmd, gx, gy, 1);
        }
    }
    barrier2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
             VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_SHADER_READ_BIT);
    if (stop == 1) { discard(); return; }

    // ---- micromaps ----
    std::vector<VkMicromapBuildInfoEXT> mbs;
    mbs.reserve(plan.size());
    for (OmmBuild& b : plan)
    {
        VkMicromapBuildInfoEXT mb{VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT};
        mb.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        mb.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        mb.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        mb.dstMicromap = mOmm[b.id].mm;
        mb.usageCountsCount = b.nUsage; mb.pUsageCounts = b.usage;
        mb.data.deviceAddress = db.address + b.dataOff;
        mb.triangleArray.deviceAddress = up.address + b.triOff;
        mb.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        mb.scratchData.deviceAddress = (takeScratch(b.mmScratch + 256) + 255) & ~VkDeviceAddress(255);
        mbs.push_back(mb);
    }
    rt.vkCmdBuildMicromapsEXT(cmd, (uint32_t)mbs.size(), mbs.data());
    barrier2(cmd, VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT, VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_2_MICROMAP_READ_BIT_EXT);
    if (stop == 2) { discard(); return; }

    // ---- OMM BLAS: the slot's triangles (same order => same primitive indices as the
    //      plain BLAS, so the shader alpha test of unknown micro-triangles still applies) ----
    std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> oms(plan.size());
    std::vector<VkAccelerationStructureGeometryKHR> geoms(plan.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(plan.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(plan.size());
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rptr(plan.size());
    for (size_t i = 0; i < plan.size(); ++i)
    {
        OmmBuild& b = plan[i];
        auto& om = oms[i];
        om = VkAccelerationStructureTrianglesOpacityMicromapEXT{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT};
        om.indexType = VK_INDEX_TYPE_UINT32; om.indexStride = 4;
        om.indexBuffer.deviceAddress = up.address + b.idxOff;
        om.usageCountsCount = b.nUsage; om.pUsageCounts = b.usage;
        om.micromap = mOmm[b.id].mm;
        auto& g = geoms[i];
        g = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        auto& t = g.geometry.triangles;
        t = VkAccelerationStructureGeometryTrianglesDataKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        t.pNext = &om;
        // Quantized positions (#573): SNORM view + the slot's decode transform, 16-bit indices
        // -- exactly the plain static BLAS's geometry (llcleanrtvk.cpp tryStatic).
        t.vertexFormat = VK_FORMAT_R16G16B16A16_SNORM;
        t.vertexData.deviceAddress = R.mVbo.address + (VkDeviceAddress)b.base_vertex * LLCleanQV::kStride;
        t.vertexStride = LLCleanQV::kStride;
        t.maxVertex = b.vert_count - 1;
        t.indexType = VK_INDEX_TYPE_UINT16;
        t.indexData.deviceAddress = R.mIbo.address + (VkDeviceAddress)b.first_index * 2;
        t.transformData.deviceAddress = up.address + b.xformOff;
        ranges[i] = VkAccelerationStructureBuildRangeInfoKHR{b.prims, 0, 0, 0};
        rptr[i] = &ranges[i];
        auto& gi = infos[i];
        gi = VkAccelerationStructureBuildGeometryInfoKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        gi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        gi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT;
        gi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        gi.dstAccelerationStructure = mOmm[b.id].as;
        gi.geometryCount = 1; gi.pGeometries = &g;
        gi.scratchData.deviceAddress = takeScratch(b.asScratch);
    }
    rt.vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), rptr.data());
    if (stop == 3) { discard(); return; }
    mOmmRecorded += (uint32_t)plan.size();
    // Live: referenced from the next frame's table (addresses uploaded then).
    for (const OmmBuild& b : plan)
    {
        OmmEntry& e = mOmm[b.id];
        e.live = true;
        ++mOmmLive;
        mOmmAddrCpu[(size_t)b.id * 4] = (uint32_t)(e.addr & 0xFFFFFFFFu);
        mOmmAddrCpu[(size_t)b.id * 4 + 1] = (uint32_t)(e.addr >> 32);
        mOmmAddrCpu[(size_t)b.id * 4 + 2] = e.key.slot;   // pass A only uses it for this slot
        mOmmAddrCpu[(size_t)b.id * 4 + 3] = 0u;
        mOmmAddrDirtyLo = std::min(mOmmAddrDirtyLo, b.id);
        mOmmAddrDirtyHi = std::max(mOmmAddrDirtyHi, b.id + 1);
    }
    plan.clear();
}
