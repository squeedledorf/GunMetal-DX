// vktest — pipeline + shader helpers (dynamic rendering, no VkRenderPass).
#pragma once

#include "vk_device.h"

#include <string>
#include <vector>

// Loads a .spv file (compiled at build time by glslc) into a shader module.
VkShaderModule loadShaderModule(VkDevice device, const std::string& spvPath);

// A small builder for a graphics pipeline that renders with dynamic rendering
// (VK_KHR_dynamic_rendering). Keeps the common defaults so scenes stay short.
struct GraphicsPipelineDesc {
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;

    // Vertex input (optional — empty means no vertex buffers, e.g. fullscreen).
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;

    VkPipelineLayout layout = VK_NULL_HANDLE;

    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth
    bool depthTest = false;
    bool depthWrite = false;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
};

VkPipeline createGraphicsPipeline(VkDevice device,
                                  const GraphicsPipelineDesc& desc);

// A compute pipeline from a single compute shader module + layout.
VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp,
                                 VkPipelineLayout layout);
