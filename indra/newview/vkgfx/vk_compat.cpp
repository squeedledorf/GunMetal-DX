// vk_compat.cpp -- see vk_compat.h (#675).
#include "vk_compat.h"

#include <cstring>
#include <type_traits>

// The wrappers below call the real entry points.
#undef vkCreateShaderModule
#undef vkGetDeviceImageMemoryRequirements

VkCompatFns gVkCompat;   // default member initializers = the loader's core entry points

bool vkCompatInstall(VkDevice device, bool api12, bool maint4, const char** why)
{
    VkCompatFns f;   // core defaults
    if (api12)
    {
        f.api12 = true;
        f.downgradeSpirv = true;
        bool ok = true;
        auto load = [&](auto& dst, const char* name, bool required)
        {
            PFN_vkVoidFunction p = vkGetDeviceProcAddr(device, name);
            if (!p)
            {
                if (required && ok)
                {
                    ok = false;
                    if (why) *why = name;
                }
                dst = nullptr;
                return;
            }
            dst = reinterpret_cast<std::remove_reference_t<decltype(dst)>>(p);
        };
        // VK_KHR_dynamic_rendering
        load(f.CmdBeginRendering, "vkCmdBeginRenderingKHR", true);
        load(f.CmdEndRendering, "vkCmdEndRenderingKHR", true);
        // VK_KHR_synchronization2
        load(f.CmdPipelineBarrier2, "vkCmdPipelineBarrier2KHR", true);
        load(f.QueueSubmit2, "vkQueueSubmit2KHR", true);
        load(f.CmdWriteTimestamp2, "vkCmdWriteTimestamp2KHR", true);
        // VK_EXT_extended_dynamic_state
        load(f.CmdSetCullMode, "vkCmdSetCullModeEXT", true);
        load(f.CmdSetFrontFace, "vkCmdSetFrontFaceEXT", true);
        load(f.CmdSetDepthTestEnable, "vkCmdSetDepthTestEnableEXT", true);
        load(f.CmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT", true);
        load(f.CmdSetDepthCompareOp, "vkCmdSetDepthCompareOpEXT", true);
        // VK_KHR_maintenance4 (optional): null = emulated.
        if (maint4)
            load(f.GetDeviceImageMemoryRequirements, "vkGetDeviceImageMemoryRequirementsKHR", false);
        else
            f.GetDeviceImageMemoryRequirements = nullptr;
        if (!ok) return false;
    }
    gVkCompat = f;
    return true;
}

bool vkCompatDowngradeSpirv(const uint32_t* code, size_t words, std::vector<uint32_t>& out)
{
    constexpr uint32_t kMagic = 0x07230203u;
    constexpr uint32_t kV15 = 0x00010500u;
    constexpr uint32_t kOpExtension = 10, kOpCapability = 17, kCapDemote = 5379;
    static const char kDemoteExt[] = "SPV_EXT_demote_to_helper_invocation";
    if (!code || words < 5 || code[0] != kMagic || code[1] <= kV15) return false;

    // Capabilities, then extensions, open the module; stop at the first other instruction.
    size_t i = 5, capsEnd = 5;
    bool demote = false, haveExt = false;
    while (i < words)
    {
        const uint32_t wc = code[i] >> 16, op = code[i] & 0xffffu;
        if (wc == 0 || i + wc > words) return false;   // malformed: leave it to the driver
        if (op == kOpCapability)
        {
            capsEnd = i + wc;
            if (wc >= 2 && code[i + 1] == kCapDemote) demote = true;
        }
        else if (op == kOpExtension)
        {
            const char* s = reinterpret_cast<const char*>(code + i + 1);
            if (std::strncmp(s, kDemoteExt, (size_t)(wc - 1) * 4) == 0) haveExt = true;
        }
        else
            break;
        i += wc;
    }

    out.assign(code, code + words);
    out[1] = kV15;
    if (demote && !haveExt)
    {
        const size_t strWords = (sizeof(kDemoteExt) + 3) / 4;   // includes the NUL
        std::vector<uint32_t> ins(1 + strWords, 0u);
        ins[0] = (uint32_t)((1 + strWords) << 16) | kOpExtension;
        std::memcpy(ins.data() + 1, kDemoteExt, sizeof(kDemoteExt));
        out.insert(out.begin() + (std::ptrdiff_t)capsEnd, ins.begin(), ins.end());
    }
    return true;
}

VkResult vkCompatCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci,
                                    const VkAllocationCallbacks* alloc, VkShaderModule* out)
{
    if (gVkCompat.downgradeSpirv && ci && ci->pCode && ci->codeSize >= 20)
    {
        std::vector<uint32_t> patched;
        if (vkCompatDowngradeSpirv(ci->pCode, ci->codeSize / 4, patched))
        {
            VkShaderModuleCreateInfo c2 = *ci;
            c2.codeSize = patched.size() * 4;
            c2.pCode = patched.data();
            return vkCreateShaderModule(dev, &c2, alloc, out);
        }
    }
    return vkCreateShaderModule(dev, ci, alloc, out);
}

void vkCompatGetDeviceImageMemoryRequirements(VkDevice dev, const VkDeviceImageMemoryRequirements* info,
                                              VkMemoryRequirements2* out)
{
    if (gVkCompat.GetDeviceImageMemoryRequirements)
    {
        gVkCompat.GetDeviceImageMemoryRequirements(dev, info, out);
        return;
    }
    // No maintenance4: the same answer from a throwaway image (no memory is bound).
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev, info->pCreateInfo, nullptr, &img) != VK_SUCCESS)
    {
        out->memoryRequirements = VkMemoryRequirements{};
        return;
    }
    VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    ri.image = img;
    vkGetImageMemoryRequirements2(dev, &ri, out);
    vkDestroyImage(dev, img, nullptr);
}
