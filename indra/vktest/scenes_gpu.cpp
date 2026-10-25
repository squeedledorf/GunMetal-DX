// vktest — Stage 5: GPU-driven cube field + #29 synchronization stress test.
//
// This file proves the target architecture the viewer memory calls for:
//   persistent instance SSBO  ->  compute frustum cull  ->
//   vkCmdDrawIndexedIndirectCount  ->  bindless per-instance textures.
//
// It also contains the #29-relevant experiment: a scene that re-uploads /
// reallocates the instance SSBO while the compute+draw are reading it, first
// WITHOUT synchronization (to see whether Vulkan sync-validation catches the
// hazard that OpenGL's implicit sync hides) and then WITH correct barriers.
#include "scenes_gpu.h"
#include "vk_pipeline.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

const glm::vec4 kClearColor(0.15f, 0.30f, 0.60f, 1.0f);

// ---- std430-matching host structs -----------------------------------------
struct Instance {
    glm::mat4 model;   // 64
    glm::vec4 sphere;  // world center + radius
    glm::uvec4 misc;   // x = texIndex
};
static_assert(sizeof(Instance) == 96, "Instance layout must match std430");

struct DrawCmd {                 // == VkDrawIndexedIndirectCommand
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;
};

struct CullPush {
    glm::vec4 planes[6];
    uint32_t instanceCount;
    uint32_t indexCount;
    uint32_t pad0, pad1;
};

struct CubeVertex { glm::vec3 pos; glm::vec3 normal; };

// ---------------------------------------------------------------------------
// Shared geometry: a unit cube (per-face normals), 24 verts / 36 indices.
// ---------------------------------------------------------------------------
void buildCube(std::vector<CubeVertex>& verts, std::vector<uint16_t>& indices) {
    const glm::vec3 fn[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    auto face = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                    glm::vec3 n) {
        uint16_t base = (uint16_t)verts.size();
        verts.push_back({a,n}); verts.push_back({b,n});
        verts.push_back({c,n}); verts.push_back({d,n});
        indices.push_back(base+0); indices.push_back(base+1); indices.push_back(base+2);
        indices.push_back(base+0); indices.push_back(base+2); indices.push_back(base+3);
    };
    const float s = 0.5f;
    face({s,-s,-s},{s,-s,s},{s,s,s},{s,s,-s}, fn[0]);
    face({-s,-s,s},{-s,-s,-s},{-s,s,-s},{-s,s,s}, fn[1]);
    face({-s,s,-s},{s,s,-s},{s,s,s},{-s,s,s}, fn[2]);
    face({-s,-s,s},{s,-s,s},{s,-s,-s},{-s,-s,-s}, fn[3]);
    face({-s,-s,s},{-s,s,s},{s,s,s},{s,-s,s}, fn[4]);
    face({s,-s,-s},{s,s,-s},{-s,s,-s},{-s,-s,-s}, fn[5]);
}

// Extract 6 frustum planes (world space) from a view-projection matrix
// (Gribb/Hartmann). Planes are normalized; inside if dot(n,p)+d >= 0.
void extractPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    // glm is column-major: m[col][row]. Build row vectors.
    glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    planes[0] = r3 + r0; // left
    planes[1] = r3 - r0; // right
    planes[2] = r3 + r1; // bottom
    planes[3] = r3 - r1; // top
    planes[4] = r2;      // near (Vulkan Z in [0,1]: near is w+z? use r3+r2? see below)
    planes[5] = r3 - r2; // far
    // For GLM_FORCE_DEPTH_ZERO_TO_ONE the near plane is row2 alone (z>=0).
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(planes[i]));
        if (len > 0.0f) planes[i] /= len;
    }
}

// A small holder for everything the GPU-driven path needs, so both the static
// scene and the stress scene can share setup.
struct GpuDriven {
    VkDeviceContext* ctx = nullptr;

    GpuBuffer vbo{}, ibo{};
    uint32_t indexCount = 0;

    GpuBuffer instanceSSBO{};        // persistent, uploaded once
    GpuBuffer instanceStaging{};     // stress only: host-visible source for
                                     // in-command-buffer re-uploads (models the
                                     // GL orphan+reupload that races the reads)
    GpuBuffer drawCmds{};            // compute output: indirect commands
    GpuBuffer drawCount{};           // compute output: single uint count
    uint32_t instanceCount = 0;
    uint32_t expectedVisible = 0;

    // Bindless textures.
    std::vector<GpuImage> textures;
    VkSampler sampler = VK_NULL_HANDLE;

    // Descriptor set 0: instance/draw SSBOs (for compute + vertex).
    VkDescriptorSetLayout dsl0 = VK_NULL_HANDLE;
    VkDescriptorSet dset0 = VK_NULL_HANDLE;
    // Descriptor set 1: bindless texture array (for fragment).
    VkDescriptorSetLayout dsl1 = VK_NULL_HANDLE;
    VkDescriptorSet dset1 = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    VkPipelineLayout cullLayout = VK_NULL_HANDLE;
    VkPipeline cullPipe = VK_NULL_HANDLE;
    VkPipelineLayout gfxLayout = VK_NULL_HANDLE;
    VkPipeline gfxPipe = VK_NULL_HANDLE;

    glm::mat4 viewProj{1.0f};
    CullPush cullPush{};

    std::vector<Instance> cpuInstances;

    void destroy();
};

// ---- procedural bindless textures: distinct solid-ish tinted checkers ------
GpuImage makeTexture(VkDeviceContext& ctx, glm::vec3 tint) {
    const uint32_t TW = 32, TH = 32;
    std::vector<uint8_t> px(TW*TH*4);
    for (uint32_t y = 0; y < TH; ++y)
        for (uint32_t x = 0; x < TW; ++x) {
            bool check = ((x/4)+(y/4)) & 1;
            float k = check ? 1.0f : 0.55f;
            uint8_t* p = &px[(y*TW+x)*4];
            p[0] = (uint8_t)(255*tint.r*k);
            p[1] = (uint8_t)(255*tint.g*k);
            p[2] = (uint8_t)(255*tint.b*k);
            p[3] = 255;
        }
    GpuImage img = ctx.createImage2D(TW, TH, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    GpuBuffer staging = ctx.createBuffer(px.size(),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, px.data(), px.size());
    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.image = img.image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d1.imageMemoryBarrierCount=1; d1.pImageMemoryBarriers=&toDst;
        vkCmdPipelineBarrier2(cmd,&d1);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent = {TW,TH,1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, img.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.image = img.image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d2.imageMemoryBarrierCount=1; d2.pImageMemoryBarriers=&toRead;
        vkCmdPipelineBarrier2(cmd,&d2);
    });
    ctx.destroyBuffer(staging);
    return img;
}

// Build the instance field. Cubes on a grid in front of the camera, plus a set
// deliberately placed BEHIND the camera (which the frustum cull must drop).
// Returns expected visible count computed on the CPU with the same test.
void buildInstances(GpuDriven& g, uint32_t gridN, uint32_t numTextures) {
    g.cpuInstances.clear();
    // Camera looks down -Z from +Z; put visible cubes at z in front, hidden
    // cubes behind the camera at large +Z.
    const float spacing = 2.0f;
    float half = (gridN-1) * 0.5f * spacing;
    uint32_t tex = 0;
    for (uint32_t j = 0; j < gridN; ++j)
        for (uint32_t i = 0; i < gridN; ++i) {
            Instance inst{};
            float x = i*spacing - half;
            float y = j*spacing - half;
            float z = -8.0f;   // in front of camera (camera at +12 looking -Z)
            inst.model = glm::translate(glm::mat4(1.0f), glm::vec3(x,y,z));
            inst.model = glm::rotate(inst.model, glm::radians(25.0f),
                                     glm::vec3(1,1,0));
            inst.sphere = glm::vec4(x,y,z, 0.87f); // sqrt(3)/2 for unit cube
            inst.misc = glm::uvec4(tex % numTextures, 0,0,0);
            ++tex;
            g.cpuInstances.push_back(inst);
        }
    // Behind-camera cubes (should be culled): place well behind the eye.
    uint32_t behind = gridN * 2;
    for (uint32_t k = 0; k < behind; ++k) {
        Instance inst{};
        float x = (float)k*spacing - half;
        inst.model = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, 30.0f));
        inst.sphere = glm::vec4(x, 0.0f, 30.0f, 0.87f);
        inst.misc = glm::uvec4(tex % numTextures, 0,0,0);
        ++tex;
        g.cpuInstances.push_back(inst);
    }
    g.instanceCount = (uint32_t)g.cpuInstances.size();
}

// Camera + planes. Camera at +12 on Z looking at origin.
void buildCamera(GpuDriven& g, uint32_t w, uint32_t h) {
    glm::mat4 view = glm::lookAt(glm::vec3(0,0,12), glm::vec3(0,0,0),
                                 glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f),
                                      (float)w/h, 0.5f, 100.0f);
    proj[1][1] *= -1.0f;
    g.viewProj = proj * view;

    // Extract planes from the *unflipped* clip matrix so the plane math matches
    // the standard convention (Y-flip only affects screen orientation, not
    // world-space visibility; but to be safe extract from the same matrix used
    // for culling decisions). Rebuild an unflipped proj for plane extraction.
    glm::mat4 projU = glm::perspective(glm::radians(60.0f),
                                       (float)w/h, 0.5f, 100.0f);
    glm::mat4 vpU = projU * view;
    extractPlanes(vpU, g.cullPush.planes);
    g.cullPush.instanceCount = g.instanceCount;
    g.cullPush.indexCount = g.indexCount;

    // CPU reference: count survivors with the same sphere-plane test.
    g.expectedVisible = 0;
    for (auto& inst : g.cpuInstances) {
        glm::vec3 c(inst.sphere);
        float r = inst.sphere.w;
        bool vis = true;
        for (int i = 0; i < 6; ++i) {
            float d = glm::dot(glm::vec3(g.cullPush.planes[i]), c)
                      + g.cullPush.planes[i].w;
            if (d < -r) { vis = false; break; }
        }
        if (vis) ++g.expectedVisible;
    }
}

void GpuDriven::destroy() {
    VkDevice dev = ctx->device();
    if (gfxPipe) vkDestroyPipeline(dev, gfxPipe, nullptr);
    if (gfxLayout) vkDestroyPipelineLayout(dev, gfxLayout, nullptr);
    if (cullPipe) vkDestroyPipeline(dev, cullPipe, nullptr);
    if (cullLayout) vkDestroyPipelineLayout(dev, cullLayout, nullptr);
    if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    if (dsl0) vkDestroyDescriptorSetLayout(dev, dsl0, nullptr);
    if (dsl1) vkDestroyDescriptorSetLayout(dev, dsl1, nullptr);
    if (sampler) vkDestroySampler(dev, sampler, nullptr);
    for (auto& t : textures) ctx->destroyImage(t);
    ctx->destroyBuffer(vbo);
    ctx->destroyBuffer(ibo);
    ctx->destroyBuffer(instanceSSBO);
    if (instanceStaging.buffer) ctx->destroyBuffer(instanceStaging);
    ctx->destroyBuffer(drawCmds);
    ctx->destroyBuffer(drawCount);
}

// Write descriptor set 0 (instance SSBO, draw cmds, draw count) — call after
// (re)allocating any of those buffers.
void writeSet0(GpuDriven& g) {
    VkDescriptorBufferInfo bi0{g.instanceSSBO.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bi1{g.drawCmds.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo bi2{g.drawCount.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = g.dset0;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    w[0].pBufferInfo = &bi0;
    w[1].pBufferInfo = &bi1;
    w[2].pBufferInfo = &bi2;
    vkUpdateDescriptorSets(g.ctx->device(), 3, w, 0, nullptr);
}

// Full setup shared by both scenes. stressReupload=false does the canonical
// upload-once device-local instance SSBO (Part A). stressReupload=true also
// creates a host-visible staging buffer AND makes the instance SSBO a
// TRANSFER_DST target, so the stress scene can re-upload it via an
// in-command-buffer vkCmdCopyBuffer that races the compute+draw reads — the
// exact analog of GL's glBufferData(orphan)+dirty-reupload-racing-reads.
void setupGpuDriven(GpuDriven& g, VkDeviceContext& ctx, const std::string& dir,
                    uint32_t w, uint32_t h, uint32_t gridN,
                    bool stressReupload) {
    g.ctx = &ctx;

    std::vector<CubeVertex> verts;
    std::vector<uint16_t> indices;
    buildCube(verts, indices);
    g.indexCount = (uint32_t)indices.size();
    g.vbo = ctx.createDeviceLocalWithData(verts.data(),
        verts.size()*sizeof(CubeVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    g.ibo = ctx.createDeviceLocalWithData(indices.data(),
        indices.size()*sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // Bindless textures (distinct tints).
    const glm::vec3 tints[4] = {
        {1.0f,0.4f,0.3f}, {0.4f,1.0f,0.5f},
        {0.4f,0.6f,1.0f}, {1.0f,0.9f,0.3f}};
    const uint32_t numTex = 4;
    for (uint32_t i = 0; i < numTex; ++i)
        g.textures.push_back(makeTexture(ctx, tints[i]));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &g.sampler));

    buildInstances(g, gridN, numTex);
    buildCamera(g, w, h);

    VkDeviceSize instBytes = g.instanceCount * sizeof(Instance);
    if (stressReupload) {
        // Device-local instance SSBO that we can re-upload INTO from a command
        // buffer via vkCmdCopyBuffer (so the write and the racing reads share
        // one submission — the true #29 model). Plus a persistent host-visible
        // staging buffer holding the "new" data each frame.
        g.instanceSSBO = ctx.createBuffer(instBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        g.instanceStaging = ctx.createBuffer(instBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        std::memcpy(g.instanceStaging.mapped, g.cpuInstances.data(), instBytes);
    } else {
        // Canonical Part A: upload once into device-local memory.
        g.instanceSSBO = ctx.createDeviceLocalWithData(g.cpuInstances.data(),
            instBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    }

    // Indirect draw command buffer + count buffer (device-local; compute writes).
    g.drawCmds = ctx.createBuffer(g.instanceCount * sizeof(DrawCmd),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    g.drawCount = ctx.createBuffer(sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // ---- Descriptor set 0: 3 storage buffers (compute + vertex) ----
    VkDescriptorSetLayoutBinding b0[3]{};
    for (int i = 0; i < 3; ++i) {
        b0[i].binding = i;
        b0[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo l0{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l0.bindingCount = 3; l0.pBindings = b0;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &l0, nullptr, &g.dsl0));

    // ---- Descriptor set 1: bindless combined-image-sampler array ----
    VkDescriptorSetLayoutBinding b1{};
    b1.binding = 0;
    b1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1.descriptorCount = numTex;
    b1.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags bf =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 1; bfi.pBindingFlags = &bf;
    VkDescriptorSetLayoutCreateInfo l1{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    l1.pNext = &bfi;
    l1.bindingCount = 1; l1.pBindings = &b1;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &l1, nullptr, &g.dsl1));

    // ---- Pool + sets ----
    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, numTex}};
    VkDescriptorPoolCreateInfo pci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 2; pci.poolSizeCount = 2; pci.pPoolSizes = psz;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &g.pool));

    VkDescriptorSetAllocateInfo a0{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    a0.descriptorPool = g.pool; a0.descriptorSetCount = 1; a0.pSetLayouts = &g.dsl0;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &a0, &g.dset0));
    VkDescriptorSetAllocateInfo a1{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    a1.descriptorPool = g.pool; a1.descriptorSetCount = 1; a1.pSetLayouts = &g.dsl1;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &a1, &g.dset1));

    writeSet0(g);

    // Bindless texture array write.
    std::vector<VkDescriptorImageInfo> imgInfos(numTex);
    for (uint32_t i = 0; i < numTex; ++i) {
        imgInfos[i].sampler = g.sampler;
        imgInfos[i].imageView = g.textures[i].view;
        imgInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet wt{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wt.dstSet = g.dset1; wt.dstBinding = 0; wt.descriptorCount = numTex;
    wt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wt.pImageInfo = imgInfos.data();
    vkUpdateDescriptorSets(ctx.device(), 1, &wt, 0, nullptr);

    // ---- Compute (cull) pipeline ----
    VkPushConstantRange cullPcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)};
    VkPipelineLayoutCreateInfo cl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cl.setLayoutCount = 1; cl.pSetLayouts = &g.dsl0;
    cl.pushConstantRangeCount = 1; cl.pPushConstantRanges = &cullPcr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &cl, nullptr, &g.cullLayout));
    VkShaderModule cs = loadShaderModule(ctx.device(), dir + "/cull.comp.spv");
    g.cullPipe = createComputePipeline(ctx.device(), cs, g.cullLayout);
    vkDestroyShaderModule(ctx.device(), cs, nullptr);

    // ---- Graphics pipeline ----
    VkDescriptorSetLayout sets[2] = {g.dsl0, g.dsl1};
    VkPushConstantRange gfxPcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo gl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    gl.setLayoutCount = 2; gl.pSetLayouts = sets;
    gl.pushConstantRangeCount = 1; gl.pPushConstantRanges = &gfxPcr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &gl, nullptr, &g.gfxLayout));

    VkShaderModule vs = loadShaderModule(ctx.device(), dir + "/gpucubes.vert.spv");
    VkShaderModule fs = loadShaderModule(ctx.device(), dir + "/gpucubes.frag.spv");
    GraphicsPipelineDesc gd;
    gd.vert = vs; gd.frag = fs; gd.layout = g.gfxLayout;
    gd.colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    gd.depthFormat = VK_FORMAT_D32_SFLOAT;
    gd.depthTest = true; gd.depthWrite = true;
    gd.cullMode = VK_CULL_MODE_NONE; // keep both windings visible for the field
    gd.bindings = {{0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    gd.attributes = {
        {0,0,VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex,pos)},
        {1,0,VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex,normal)}};
    g.gfxPipe = createGraphicsPipeline(ctx.device(), gd);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
}

// Record the begin-rendering barriers + clears for color + depth.
void beginRenderingCD(VkCommandBuffer cmd, OffscreenTarget& t) {
    uint32_t w = t.width(), h = t.height();
    VkImageMemoryBarrier2 bar[2];
    VkImageMemoryBarrier2 col{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    col.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    col.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    col.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    col.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    col.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    col.image = t.color().image;
    col.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    bar[0] = col;
    VkImageMemoryBarrier2 d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    d.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    d.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT
                     | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    d.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                      | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    d.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    d.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    d.image = t.depth().image;
    d.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    bar[1] = d;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2; dep.pImageMemoryBarriers = bar;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = t.color().view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{kClearColor.r,kClearColor.g,kClearColor.b,kClearColor.a}};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = t.depth().view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0,0},{w,h}};
    ri.layerCount = 1; ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0,0,(float)w,(float)h,0,1};
    VkRect2D sc{{0,0},{w,h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// Record one GPU-driven frame. Optional in-command-buffer re-upload of the
// instance SSBO (models GL orphan+dirty-reupload). `sync` gates ALL the
// intra-frame barriers (instance-copy->read, fill->read, compute->draw). With
// sync=false the frame is the intentional #29 hazard: a device buffer written
// then immediately read by compute+draw in the same submission, unsynchronized.
void recordFrame(VkCommandBuffer cmd, GpuDriven& g, OffscreenTarget& t,
                 bool sync, bool reuploadInstances) {
    const bool withComputeBarrier = sync;

    // Zero the draw count FIRST, and always correctly barrier it into compute,
    // so this scaffolding never contributes a hazard. (Done before the instance
    // re-upload so that in the hazard case NOTHING sits between the instance
    // copy-write and the compute read — keeping the instance RAW hazard the sole
    // uncovered dependency, exactly modelling #29.)
    vkCmdFillBuffer(cmd, g.drawCount.buffer, 0, sizeof(uint32_t), 0);
    {
        VkBufferMemoryBarrier2 fillBar{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        fillBar.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        fillBar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fillBar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                               | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        fillBar.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT
                                | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                                | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        fillBar.buffer = g.drawCount.buffer; fillBar.size = VK_WHOLE_SIZE;
        VkDependencyInfo fdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        fdep.bufferMemoryBarrierCount = 1; fdep.pBufferMemoryBarriers = &fillBar;
        vkCmdPipelineBarrier2(cmd, &fdep);
    }

    // ---- #29 model: re-upload the instance SSBO from staging, in-cmd ----
    if (reuploadInstances) {
        VkBufferCopy region{0, 0, g.instanceCount * sizeof(Instance)};
        vkCmdCopyBuffer(cmd, g.instanceStaging.buffer, g.instanceSSBO.buffer,
                        1, &region);
        if (sync) {
            // Correct: transfer-write -> compute+vertex shader-read.
            VkBufferMemoryBarrier2 ib{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            ib.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                              | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            ib.buffer = g.instanceSSBO.buffer; ib.size = VK_WHOLE_SIZE;
            VkDependencyInfo idep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            idep.bufferMemoryBarrierCount = 1; idep.pBufferMemoryBarriers = &ib;
            vkCmdPipelineBarrier2(cmd, &idep);
        }
        // else: NO barrier — compute + vertex read while/after the copy writes,
        // with no dependency. This is the WAR/RAW hazard #29 is about.
    }

    // Compute cull.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g.cullLayout,
                            0, 1, &g.dset0, 0, nullptr);
    vkCmdPushConstants(cmd, g.cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(CullPush), &g.cullPush);
    uint32_t groups = (g.instanceCount + 63) / 64;
    vkCmdDispatch(cmd, groups, 1, 1);

    if (withComputeBarrier) {
        // Compute writes (draw cmds + count) -> indirect + vertex reads.
        VkBufferMemoryBarrier2 b2{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b2.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b2.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b2.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
                          | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b2.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT
                           | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        b2.buffer = g.drawCmds.buffer; b2.size = VK_WHOLE_SIZE;
        VkBufferMemoryBarrier2 b3 = b2;
        b3.buffer = g.drawCount.buffer;
        VkBufferMemoryBarrier2 bufs[2] = {b2, b3};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = bufs;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // Draw.
    beginRenderingCD(cmd, t);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.gfxPipe);
    VkDescriptorSet sets[2] = {g.dset0, g.dset1};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.gfxLayout,
                            0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, g.gfxLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(glm::mat4), &g.viewProj);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g.vbo.buffer, &off);
    vkCmdBindIndexBuffer(cmd, g.ibo.buffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexedIndirectCount(cmd, g.drawCmds.buffer, 0,
                                  g.drawCount.buffer, 0,
                                  g.instanceCount, sizeof(DrawCmd));
    vkCmdEndRendering(cmd);
}

// Read back the compute-produced draw count to the CPU for verification.
uint32_t readDrawCount(GpuDriven& g) {
    GpuBuffer host = g.ctx->createBuffer(sizeof(uint32_t),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g.ctx->submitImmediate([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0,0,sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, g.drawCount.buffer, host.buffer, 1, &region);
    });
    uint32_t val = 0;
    std::memcpy(&val, host.mapped, sizeof(uint32_t));
    g.ctx->destroyBuffer(host);
    return val;
}

} // namespace

// ---------------------------------------------------------------------------
// PART A — static GPU-driven cube field.
// ---------------------------------------------------------------------------
bool sceneGpuCubes(VkDeviceContext& ctx, OffscreenTarget& t,
                   const std::string& dir) {
    GpuDriven g;
    setupGpuDriven(g, ctx, dir, t.width(), t.height(),
                   /*gridN=*/16, /*stressReupload=*/false);

    VK_LOG("gpucubes: %u instances (%u visible-expected on CPU, "
           "including behind-camera cubes that must be culled)",
           g.instanceCount, g.expectedVisible);

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        recordFrame(cmd, g, t, /*sync=*/true, /*reuploadInstances=*/false);
    });

    uint32_t drawn = readDrawCount(g);
    VK_LOG("gpucubes: compute emitted %u draws (CPU expected %u) -> %s",
           drawn, g.expectedVisible,
           drawn == g.expectedVisible ? "MATCH" : "MISMATCH");

    g.destroy();
    if (drawn != g.expectedVisible) {
        std::fprintf(stderr, "[vktest] gpucubes: drawn count mismatch\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PART B — synchronization stress test (#29-relevant).
//
// Two phases against the offscreen target, many simulated frames each:
//   Phase 1 (HAZARD): each frame host-re-uploads the WHOLE instance SSBO
//     (mapped, host-coherent) and immediately dispatches compute+draw that
//     READ it, with NO barrier between the host write and the shader reads,
//     and NO barrier between compute writes and the indirect/vertex reads.
//     Sync-validation warnings here are the EXPECTED result.
//   Phase 2 (CORRECT): same work, but with a host-write->shader-read barrier
//     and the compute->draw barrier in place. Sync-val must go silent.
//
// The scene reports (to stdout, clearly labeled) the validation tallies for
// each phase; main() treats gpucubes_stress specially so phase-1 warnings do
// not abort the experiment.
// ---------------------------------------------------------------------------
bool sceneGpuCubesStress(VkDeviceContext& ctx, OffscreenTarget& t,
                         const std::string& dir, GpuStressResult& out) {
    GpuDriven g;
    setupGpuDriven(g, ctx, dir, t.width(), t.height(),
                   /*gridN=*/12, /*stressReupload=*/true);
    const uint32_t kFrames = 32;

    // ---------------- Phase 1: intentional hazard ----------------
    VK_LOG("==== gpucubes_stress PHASE 1: UNSYNCHRONIZED (intentional hazard) ====");
    VK_LOG("Each frame, in ONE command buffer: vkCmdCopyBuffer re-uploads the");
    VK_LOG("instance SSBO (models GL glBufferData orphan + dirty reupload), then");
    VK_LOG("IMMEDIATELY dispatches the compute cull + indirect draw that READ it,");
    VK_LOG("with NO barrier between the copy-write and the compute/vertex reads,");
    VK_LOG("and NO compute-write -> draw-read barrier. Running %u frames, "
           "sync-val ON.", kFrames);
    uint32_t errBefore = ctx.validationErrorCount();
    uint32_t warnBefore = ctx.validationWarnCount();

    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        // Vary the staging data each frame so the re-upload is "live".
        for (auto& inst : g.cpuInstances)
            inst.misc.x = (inst.misc.x + 1u) & 3u;
        std::memcpy(g.instanceStaging.mapped, g.cpuInstances.data(),
                    g.instanceCount * sizeof(Instance));
        ctx.submitImmediate([&](VkCommandBuffer cmd) {
            recordFrame(cmd, g, t, /*sync=*/false, /*reuploadInstances=*/true);
        });
    }
    out.hazardErrors = ctx.validationErrorCount() - errBefore;
    out.hazardWarnings = ctx.validationWarnCount() - warnBefore;
    VK_LOG("PHASE 1 result: %u validation error(s), %u warning(s) during the "
           "unsynchronized frames.", out.hazardErrors, out.hazardWarnings);

    // ---------------- Phase 2: corrected ----------------
    VK_LOG("==== gpucubes_stress PHASE 2: SYNCHRONIZED (corrected) ====");
    VK_LOG("Same per-frame in-cmd re-upload, but now with a transfer-write ->");
    VK_LOG("shader-read barrier on the instance SSBO AND the compute-write ->");
    VK_LOG("draw-read barrier in place. Running %u frames.", kFrames);
    errBefore = ctx.validationErrorCount();
    warnBefore = ctx.validationWarnCount();

    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        for (auto& inst : g.cpuInstances)
            inst.misc.x = (inst.misc.x + 1u) & 3u;
        std::memcpy(g.instanceStaging.mapped, g.cpuInstances.data(),
                    g.instanceCount * sizeof(Instance));
        ctx.submitImmediate([&](VkCommandBuffer cmd) {
            recordFrame(cmd, g, t, /*sync=*/true, /*reuploadInstances=*/true);
        });
    }
    out.correctErrors = ctx.validationErrorCount() - errBefore;
    out.correctWarnings = ctx.validationWarnCount() - warnBefore;
    VK_LOG("PHASE 2 result: %u validation error(s), %u warning(s) during the "
           "synchronized frames.", out.correctErrors, out.correctWarnings);

    out.finalDrawCount = readDrawCount(g);
    out.expectedVisible = g.expectedVisible;
    VK_LOG("PHASE 2 final draw count = %u (CPU expected %u)",
           out.finalDrawCount, out.expectedVisible);

    g.destroy();
    // The scene "succeeds" as an experiment as long as it ran; correctness of
    // phase 2 is asserted by main via the result struct.
    return true;
}
