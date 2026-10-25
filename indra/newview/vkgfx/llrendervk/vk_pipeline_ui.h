// LLRenderVK — UI pipeline builder.
//
// Separate from the vktest spike's generic createGraphicsPipeline() because the
// immediate-mode emulator needs per-batch control over PRIMITIVE TOPOLOGY and
// ALPHA BLEND state (the two axes that key our pipeline cache), plus a fixed
// interleaved pos/color/uv vertex layout. Dynamic rendering, no VkRenderPass.
#pragma once

#include "vk_common.h"

#include <string>

VkShaderModule loadUiShaderModule(VkDevice device, const std::string& spvPath);

struct UiPipelineDesc {
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    bool blendEnable = true;
    VkBlendFactor srcFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    VkBlendFactor dstFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    // Alpha blend factors: false = the UI's coverage accumulation (ONE,
    // ONE_MINUS_SRC_ALPHA); true = the colour factors, as GL's glBlendFunc does
    // (offscreen render targets: the avatar compositor's alpha passes depend on it).
    bool glAlphaBlend = false;

    // Color write mask (bit0=R,1=G,2=B,3=A) baked into the blend attachment.
    uint8_t colorWriteMask = 0xF;

    // Depth test/write/compare. Baked in; requires the target to have a depth
    // attachment when depthTest is true. depthFormat must be set in that case.
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    // Depth bias toward the camera (world overlays drawn ON surfaces -- property
    // lines hug the terrain, parcel highlights the ground): slope-scaled so the
    // overlay wins the LEQUAL test at grazing angles instead of z-fighting away.
    uint8_t depthBias = 0;   // 1: toward the camera, conventional depth; 2: reversed-Z

    // Dynamic line width (enables VK_DYNAMIC_STATE_LINE_WIDTH). Requires the
    // wideLines device feature if the width ever exceeds 1.0.
    bool dynamicLineWidth = false;

    // Interleaved vertex layout.
    uint32_t stride = 0;
    uint32_t posOffset = 0;
    uint32_t colOffset = 0;
    uint32_t uvOffset = 0;
};

VkPipeline createUiPipeline(VkDevice device, const UiPipelineDesc& d);
