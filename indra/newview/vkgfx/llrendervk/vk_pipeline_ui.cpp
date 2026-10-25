#include "vk_pipeline_ui.h"
#include "vk_pipeline.h"   // #672 vkCreatePipelineResilient

#include <cstdio>
#include <vector>

VkShaderModule loadUiShaderModule(VkDevice device, const std::string& spvPath) {
    FILE* f = std::fopen(spvPath.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[llrendervk] cannot open shader %s\n",
                     spvPath.c_str());
        std::abort();
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<char> code(len);
    if (std::fread(code.data(), 1, len, f) != (size_t)len) std::abort();
    std::fclose(f);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = (size_t)len;
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &mod));
    return mod;
}

VkPipeline createUiPipeline(VkDevice device, const UiPipelineDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d.vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = d.frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{0, d.stride,
                                         VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, d.posOffset},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, d.colOffset},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, d.uvOffset},
    };
    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = d.topology;

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // UI is 2D; no culling
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    if (d.depthBias && d.depthTest)
    {
        // Toward the camera: negative with standard Z (smaller = nearer), positive
        // with reversed-Z (larger = nearer).
        const float s = d.depthBias == 2 ? 1.0f : -1.0f;
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = 16.0f * s;
        rs.depthBiasSlopeFactor = 4.0f * s;
    }

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depthTest ? d.depthCompareOp : VK_COMPARE_OP_ALWAYS;

    VkColorComponentFlags mask = 0;
    if (d.colorWriteMask & 1) mask |= VK_COLOR_COMPONENT_R_BIT;
    if (d.colorWriteMask & 2) mask |= VK_COLOR_COMPONENT_G_BIT;
    if (d.colorWriteMask & 4) mask |= VK_COLOR_COMPONENT_B_BIT;
    if (d.colorWriteMask & 8) mask |= VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = mask;
    cba.blendEnable = d.blendEnable ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = d.srcFactor;
    cba.dstColorBlendFactor = d.dstFactor;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    // Alpha: accumulate coverage as src.a + dst.a*(1-src.a) so overlapping
    // translucent panels build up a sensible framebuffer alpha.
    cba.srcAlphaBlendFactor = d.glAlphaBlend ? d.srcFactor : VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = d.glAlphaBlend ? d.dstFactor : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT,
                             VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_LINE_WIDTH};
    VkPipelineDynamicStateCreateInfo dynInfo{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynInfo.dynamicStateCount = d.dynamicLineWidth ? 3 : 2;
    dynInfo.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &d.colorFormat;
    // Declare the depth format whenever the render pass has a depth attachment,
    // even if this pipeline doesn't test depth (dynamic-rendering format match).
    if (d.depthFormat != VK_FORMAT_UNDEFINED)
        rendering.depthAttachmentFormat = d.depthFormat;

    VkGraphicsPipelineCreateInfo pci{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dynInfo;
    pci.layout = d.layout;

    // #672: out of memory is retried (vk_pipeline.h); a pipeline still failing comes back
    // VK_NULL_HANDLE (its binds / draws are skipped) instead of a fatal error.
    struct Ctx { VkDevice device; const VkGraphicsPipelineCreateInfo* pci; } ctx{device, &pci};
    return vkCreatePipelineResilient(
        "UI pipeline",
        [](void* c, VkPipeline* out) {
            const Ctx& x = *static_cast<const Ctx*>(c);
            return vkCreateGraphicsPipelines(x.device, vkSharedPipelineCache(), 1, x.pci, nullptr, out);
        },
        &ctx, "vkCreateGraphicsPipelines(device, vkSharedPipelineCache(), 1, &pci, nullptr, &pipeline)", __FILE__,
        __LINE__);
}
