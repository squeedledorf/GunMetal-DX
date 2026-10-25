// vk_compat.h -- Vulkan 1.3 core -> 1.2 + extensions entry-point routing (#675).
//
// The renderer is written against the Vulkan 1.3 core entry points (dynamic rendering,
// synchronization2, extended dynamic state, maintenance4). On a device that reports
// >= 1.3 NOTHING changes: every pointer in gVkCompat defaults to the loader's own
// exported core function, and vkCompatInstall(..., api12 = false) leaves them (or
// restores them) that way. On a Vulkan 1.2 device (or with RenderVKForceApi12 /
// SLIPSTREAM_VK_FORCE_API12 on a 1.3 one) vkCompatInstall points the same names at the
// promoted-from extensions' entry points:
//
//   vkCmdBeginRendering / EndRendering          -> VK_KHR_dynamic_rendering
//   vkCmdPipelineBarrier2 / vkQueueSubmit2 /
//   vkCmdWriteTimestamp2                         -> VK_KHR_synchronization2
//   vkCmdSetCullMode / FrontFace / DepthTestEnable /
//   DepthWriteEnable / DepthCompareOp            -> VK_EXT_extended_dynamic_state
//   vkGetDeviceImageMemoryRequirements           -> VK_KHR_maintenance4, else emulated
//                                                   (temporary VkImage + vkGetImageMemoryRequirements2)
//   vkCreateShaderModule                         -> SPIR-V 1.6 modules are down-versioned
//                                                   to 1.5 (see vkCompatDowngradeSpirv)
//
// Call sites are untouched: the macros below (active in every TU that includes
// vk_common.h) redirect the core names through gVkCompat. The structs involved
// (VkRenderingInfo, VkDependencyInfo, VkSubmitInfo2 ...) are identical to their KHR
// aliases, so no data changes.
//
// The macros are function-like: a bare name (`PFN_vkX p = vkX;`, `&vkX`) is NOT expanded.
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct VkCompatFns
{
    // Defaults = the loader's exported core functions (the Vulkan 1.3 path).
    PFN_vkCmdBeginRendering              CmdBeginRendering              = vkCmdBeginRendering;
    PFN_vkCmdEndRendering                CmdEndRendering                = vkCmdEndRendering;
    PFN_vkCmdPipelineBarrier2            CmdPipelineBarrier2            = vkCmdPipelineBarrier2;
    PFN_vkQueueSubmit2                   QueueSubmit2                   = vkQueueSubmit2;
    PFN_vkCmdWriteTimestamp2             CmdWriteTimestamp2             = vkCmdWriteTimestamp2;
    PFN_vkCmdSetCullMode                 CmdSetCullMode                 = vkCmdSetCullMode;
    PFN_vkCmdSetFrontFace                CmdSetFrontFace                = vkCmdSetFrontFace;
    PFN_vkCmdSetDepthTestEnable          CmdSetDepthTestEnable          = vkCmdSetDepthTestEnable;
    PFN_vkCmdSetDepthWriteEnable         CmdSetDepthWriteEnable         = vkCmdSetDepthWriteEnable;
    PFN_vkCmdSetDepthCompareOp           CmdSetDepthCompareOp           = vkCmdSetDepthCompareOp;
    PFN_vkGetDeviceImageMemoryRequirements GetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;
    bool                                 downgradeSpirv                 = false;   // true on the 1.2 path
    bool                                 api12                          = false;
};
extern VkCompatFns gVkCompat;

// Points gVkCompat at the right entry points for `device`. api12 = false restores the
// core defaults (nothing is looked up). api12 = true resolves the KHR / EXT names with
// vkGetDeviceProcAddr; returns false and names the first missing entry point in `why`
// when one is absent (the extension was enabled, so the driver is broken).
// maint4 = VK_KHR_maintenance4 was enabled (else the image requirements query is emulated).
bool vkCompatInstall(VkDevice device, bool api12, bool maint4, const char** why);

// Rewrites a SPIR-V 1.6 module so a Vulkan 1.2 device accepts it: version word 1.6 -> 1.5,
// and OpExtension SPV_EXT_demote_to_helper_invocation added when the module declares
// the DemoteToHelperInvocation capability (core in 1.6, an extension in 1.5; the device
// enables VK_EXT_shader_demote_to_helper_invocation). Returns false (out untouched)
// when the module needs no change.
bool vkCompatDowngradeSpirv(const uint32_t* code, size_t words, std::vector<uint32_t>& out);

VkResult vkCompatCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci,
                                    const VkAllocationCallbacks* alloc, VkShaderModule* out);
void vkCompatGetDeviceImageMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements* info,
                                              VkMemoryRequirements2* out);

// Hot paths: a direct call through the pointer (identical callee on the 1.3 path).
#define vkCmdBeginRendering(...)          (gVkCompat.CmdBeginRendering(__VA_ARGS__))
#define vkCmdEndRendering(...)            (gVkCompat.CmdEndRendering(__VA_ARGS__))
#define vkCmdWriteTimestamp2(...)         (gVkCompat.CmdWriteTimestamp2(__VA_ARGS__))
#define vkCmdSetCullMode(...)             (gVkCompat.CmdSetCullMode(__VA_ARGS__))
#define vkCmdSetFrontFace(...)            (gVkCompat.CmdSetFrontFace(__VA_ARGS__))
#define vkCmdSetDepthTestEnable(...)      (gVkCompat.CmdSetDepthTestEnable(__VA_ARGS__))
#define vkCmdSetDepthWriteEnable(...)     (gVkCompat.CmdSetDepthWriteEnable(__VA_ARGS__))
#define vkCmdSetDepthCompareOp(...)       (gVkCompat.CmdSetDepthCompareOp(__VA_ARGS__))
// Not hot (setup / accounting): a wrapper.
#define vkGetDeviceImageMemoryRequirements(...) vkCompatGetDeviceImageMemoryRequirements(__VA_ARGS__)
#define vkCreateShaderModule(...)         vkCompatCreateShaderModule(__VA_ARGS__)
// vkCmdPipelineBarrier2 / vkQueueSubmit2 are already routed through vk_common.h's hooks
// (vkqCmdPipelineBarrier2 / vkqQueueSubmit2); those two call gVkCompat directly.
