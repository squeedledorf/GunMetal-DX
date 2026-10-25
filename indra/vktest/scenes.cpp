#include "scenes.h"
#include "vk_pipeline.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE   // Vulkan clip Z is [0,1], not [-1,1].
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <string>
#include <vector>

namespace {

const glm::vec4 kClearColor(0.15f, 0.30f, 0.60f, 1.0f);

// ---------------------------------------------------------------------------
// Common dynamic-rendering begin/end. Transitions the color (and depth) image
// from UNDEFINED to the attachment layout, begins rendering with a clear, and
// sets viewport/scissor. Returns after vkCmdBeginRendering.
// ---------------------------------------------------------------------------
void beginRendering(VkCommandBuffer cmd, OffscreenTarget& t) {
    uint32_t w = t.width(), h = t.height();

    std::vector<VkImageMemoryBarrier2> barriers;
    VkImageMemoryBarrier2 colBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    colBar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    colBar.srcAccessMask = 0;
    colBar.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    colBar.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    colBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colBar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colBar.image = t.color().image;
    colBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers.push_back(colBar);

    if (t.hasDepth()) {
        VkImageMemoryBarrier2 dBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        dBar.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        dBar.srcAccessMask = 0;
        dBar.dstStageMask =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        dBar.dstAccessMask =
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        dBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dBar.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        dBar.image = t.depth().image;
        dBar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        barriers.push_back(dBar);
    }

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = (uint32_t)barriers.size();
    dep.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = t.color().view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{kClearColor.r, kClearColor.g, kClearColor.b,
                               kClearColor.a}};

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (t.hasDepth()) {
        depth.imageView = t.depth().view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
    }

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    if (t.hasDepth()) ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);

    VkViewport vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// ---------------------------------------------------------------------------
// Scene 1: clear
// ---------------------------------------------------------------------------
bool sceneClear(VkDeviceContext& ctx, OffscreenTarget& t) {
    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        beginRendering(cmd, t);
        vkCmdEndRendering(cmd);
    });
    return true;
}

// ---------------------------------------------------------------------------
// Scene 2: triangle
// ---------------------------------------------------------------------------
bool sceneTriangle(VkDeviceContext& ctx, OffscreenTarget& t,
                   const std::string& dir) {
    VkShaderModule vs = loadShaderModule(ctx.device(), dir + "/triangle.vert.spv");
    VkShaderModule fs = loadShaderModule(ctx.device(), dir + "/triangle.frag.spv");

    VkPipelineLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout));

    GraphicsPipelineDesc d;
    d.vert = vs; d.frag = fs; d.layout = layout;
    d.colorFormat = t.colorFormat();
    VkPipeline pipe = createGraphicsPipeline(ctx.device(), d);

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        beginRendering(cmd, t);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    });

    vkDestroyPipeline(ctx.device(), pipe, nullptr);
    vkDestroyPipelineLayout(ctx.device(), layout, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Scene 3: textured quad
// ---------------------------------------------------------------------------
bool sceneTexquad(VkDeviceContext& ctx, OffscreenTarget& t,
                  const std::string& dir) {
    // Build a 64x64 procedural test texture: checkerboard tinted by a UV
    // gradient so both the sampler and the UV mapping are verifiable.
    const uint32_t TW = 64, TH = 64;
    std::vector<uint8_t> pixels(TW * TH * 4);
    for (uint32_t y = 0; y < TH; ++y) {
        for (uint32_t x = 0; x < TW; ++x) {
            bool check = ((x / 8) + (y / 8)) & 1;
            uint8_t* p = &pixels[(y * TW + x) * 4];
            uint8_t base = check ? 230 : 40;
            p[0] = (uint8_t)(base * (x / (float)(TW - 1)));       // R grows with U
            p[1] = (uint8_t)(base * (y / (float)(TH - 1)));       // G grows with V
            p[2] = base;                                          // B checker
            p[3] = 255;
        }
    }

    GpuImage tex = ctx.createImage2D(
        TW, TH, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);

    GpuBuffer staging = ctx.createBuffer(
        pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.image = tex.image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d1.imageMemoryBarrierCount = 1; d1.pImageMemoryBarriers = &toDst;
        vkCmdPipelineBarrier2(cmd, &d1);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {TW, TH, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier2 toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.image = tex.image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d2.imageMemoryBarrierCount = 1; d2.pImageMemoryBarriers = &toRead;
        vkCmdPipelineBarrier2(cmd, &d2);
    });
    ctx.destroyBuffer(staging);

    // Sampler (nearest so checker edges stay crisp for verification).
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &sampler));

    // Descriptor set layout: one combined image sampler at binding 0.
    VkDescriptorSetLayoutBinding b0{};
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 1; dslci.pBindings = &b0;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dslci, nullptr, &dsl));

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &psz;
    VkDescriptorPool pool;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &pool));

    VkDescriptorSetAllocateInfo dsai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool; dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    VkDescriptorSet dset;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsai, &dset));

    VkDescriptorImageInfo dii{};
    dii.sampler = sampler;
    dii.imageView = tex.view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = dset; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(ctx.device(), 1, &wr, 0, nullptr);

    VkPipelineLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1; lci.pSetLayouts = &dsl;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout));

    VkShaderModule vs = loadShaderModule(ctx.device(), dir + "/texquad.vert.spv");
    VkShaderModule fs = loadShaderModule(ctx.device(), dir + "/texquad.frag.spv");
    GraphicsPipelineDesc gd;
    gd.vert = vs; gd.frag = fs; gd.layout = layout;
    gd.colorFormat = t.colorFormat();
    VkPipeline pipe = createGraphicsPipeline(ctx.device(), gd);

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        beginRendering(cmd, t);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                0, 1, &dset, 0, nullptr);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRendering(cmd);
    });

    vkDestroyPipeline(ctx.device(), pipe, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
    vkDestroyPipelineLayout(ctx.device(), layout, nullptr);
    vkDestroyDescriptorPool(ctx.device(), pool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device(), dsl, nullptr);
    vkDestroySampler(ctx.device(), sampler, nullptr);
    ctx.destroyImage(tex);
    return true;
}

// ---------------------------------------------------------------------------
// Scene 4: lit cube
// ---------------------------------------------------------------------------
struct CubeVertex { glm::vec3 pos; glm::vec3 normal; };

bool sceneLitcube(VkDeviceContext& ctx, OffscreenTarget& t,
                  const std::string& dir) {
    // Unit cube centered at origin. Per-face vertices so each face has a flat
    // outward normal (24 verts, 36 indices).
    const glm::vec3 faceNormals[6] = {
        { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0},
        { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}};
    // Face vertex corners (CCW when viewed from outside).
    auto pushFace = [](std::vector<CubeVertex>& v, std::vector<uint16_t>& idx,
                       glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d,
                       glm::vec3 n) {
        uint16_t base = (uint16_t)v.size();
        v.push_back({a, n}); v.push_back({b, n});
        v.push_back({c, n}); v.push_back({d, n});
        idx.push_back(base + 0); idx.push_back(base + 1); idx.push_back(base + 2);
        idx.push_back(base + 0); idx.push_back(base + 2); idx.push_back(base + 3);
    };
    std::vector<CubeVertex> verts;
    std::vector<uint16_t> indices;
    const float s = 0.5f;
    // +X
    pushFace(verts, indices, {s,-s,-s},{s,-s,s},{s,s,s},{s,s,-s}, faceNormals[0]);
    // -X
    pushFace(verts, indices, {-s,-s,s},{-s,-s,-s},{-s,s,-s},{-s,s,s}, faceNormals[1]);
    // +Y
    pushFace(verts, indices, {-s,s,-s},{s,s,-s},{s,s,s},{-s,s,s}, faceNormals[2]);
    // -Y
    pushFace(verts, indices, {-s,-s,s},{s,-s,s},{s,-s,-s},{-s,-s,-s}, faceNormals[3]);
    // +Z
    pushFace(verts, indices, {-s,-s,s},{-s,s,s},{s,s,s},{s,-s,s}, faceNormals[4]);
    // -Z
    pushFace(verts, indices, {s,-s,-s},{s,s,-s},{-s,s,-s},{-s,-s,-s}, faceNormals[5]);

    GpuBuffer vbo = ctx.createDeviceLocalWithData(
        verts.data(), verts.size() * sizeof(CubeVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    GpuBuffer ibo = ctx.createDeviceLocalWithData(
        indices.data(), indices.size() * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // MVP. GLM_FORCE_DEPTH_ZERO_TO_ONE makes perspective produce Vulkan Z.
    // We flip Y in the projection (proj[1][1] *= -1) so +Y is up in the PNG.
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(25.0f),
                                  glm::vec3(0, 1, 0));
    model = glm::rotate(model, glm::radians(20.0f), glm::vec3(1, 0, 0));
    glm::mat4 view = glm::lookAt(glm::vec3(2.2f, 1.8f, 2.2f),
                                 glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(35.0f),
                                      (float)t.width() / t.height(),
                                      0.1f, 100.0f);
    proj[1][1] *= -1.0f; // Vulkan: flip Y
    struct PushConstants { glm::mat4 mvp; glm::mat4 model; } pc;
    pc.mvp = proj * view * model;
    pc.model = model;

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &lci, nullptr, &layout));

    VkShaderModule vs = loadShaderModule(ctx.device(), dir + "/litcube.vert.spv");
    VkShaderModule fs = loadShaderModule(ctx.device(), dir + "/litcube.frag.spv");

    GraphicsPipelineDesc gd;
    gd.vert = vs; gd.frag = fs; gd.layout = layout;
    gd.colorFormat = t.colorFormat();
    gd.depthFormat = t.depthFormat();
    gd.depthTest = true; gd.depthWrite = true;
    gd.cullMode = VK_CULL_MODE_BACK_BIT;
    // The projection Y-flip (proj[1][1] *= -1) reverses triangle winding in
    // screen space, so our CCW-authored faces present as clockwise. Declare
    // CLOCKWISE as front-facing so back-face culling keeps the near faces
    // (whose outward normals face the camera/light), not the far ones.
    gd.frontFace = VK_FRONT_FACE_CLOCKWISE;
    gd.bindings = {{0, sizeof(CubeVertex), VK_VERTEX_INPUT_RATE_VERTEX}};
    gd.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeVertex, normal)}};
    VkPipeline pipe = createGraphicsPipeline(ctx.device(), gd);

    ctx.submitImmediate([&](VkCommandBuffer cmd) {
        beginRendering(cmd, t);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(PushConstants), &pc);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbo.buffer, &off);
        vkCmdBindIndexBuffer(cmd, ibo.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, (uint32_t)indices.size(), 1, 0, 0, 0);
        vkCmdEndRendering(cmd);
    });

    vkDestroyPipeline(ctx.device(), pipe, nullptr);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
    vkDestroyPipelineLayout(ctx.device(), layout, nullptr);
    ctx.destroyBuffer(vbo);
    ctx.destroyBuffer(ibo);
    return true;
}

} // namespace

bool renderScene(const std::string& name, VkDeviceContext& ctx,
                 OffscreenTarget& target, const std::string& shaderDir) {
    if (name == "clear")   return sceneClear(ctx, target);
    if (name == "triangle")return sceneTriangle(ctx, target, shaderDir);
    if (name == "texquad") return sceneTexquad(ctx, target, shaderDir);
    if (name == "litcube") return sceneLitcube(ctx, target, shaderDir);
    std::fprintf(stderr, "[vktest] unknown scene '%s'\n", name.c_str());
    return false;
}
