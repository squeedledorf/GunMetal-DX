// vk_descpool.cpp -- see vk_descpool.h.
#include "linden_common.h"

#include "vk_descpool.h"
#include "vk_common.h"

#include "llerror.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>

// ===========================================================================
// type counts
// ===========================================================================
uint64_t VkdTypeCounts::get(VkDescriptorType t) const
{
    for (const auto& p : v)
        if (p.first == t) return p.second;
    return 0;
}

void VkdTypeCounts::add(VkDescriptorType t, uint64_t n)
{
    if (!n) return;
    for (auto& p : v)
        if (p.first == t) { p.second += n; return; }
    v.emplace_back(t, n);
}

void VkdTypeCounts::sub(VkDescriptorType t, uint64_t n)
{
    for (auto& p : v)
        if (p.first == t) { p.second = p.second > n ? p.second - n : 0; return; }
}

namespace
{
const char* typeName(VkDescriptorType t)
{
    switch (t)
    {
    case VK_DESCRIPTOR_TYPE_SAMPLER:                    return "SAMPLER";
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:     return "COMBINED_IMAGE_SAMPLER";
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:              return "SAMPLED_IMAGE";
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:              return "STORAGE_IMAGE";
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:       return "UNIFORM_TEXEL_BUFFER";
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:       return "STORAGE_TEXEL_BUFFER";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:             return "UNIFORM_BUFFER";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:             return "STORAGE_BUFFER";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:     return "UNIFORM_BUFFER_DYNAMIC";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:     return "STORAGE_BUFFER_DYNAMIC";
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:           return "INPUT_ATTACHMENT";
    case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:       return "INLINE_UNIFORM_BLOCK(bytes)";
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "ACCELERATION_STRUCTURE_KHR";
    default:                                            return "OTHER";
    }
}

// Both LL log and stderr: VK_CHECK aborts right after a failed allocation and the
// tester's paste is usually the [vktest] stderr tail.
void warnBoth(const std::string& msg)
{
    LL_WARNS("Vulkan") << msg << LL_ENDL;
    std::fprintf(stderr, "[vktest] %s\n", msg.c_str());
    std::fflush(stderr);
}

struct Binding
{
    VkDescriptorType   type;
    uint32_t           count;      // declared (maximum for a variable-count binding)
    VkShaderStageFlags stages;
    bool               variable;   // VARIABLE_DESCRIPTOR_COUNT_BIT
};

struct LayoutInfo
{
    const char*          name = "?";
    std::vector<Binding> bindings;
    bool                 uab = false;     // needs an UPDATE_AFTER_BIND pool
    bool                 hasVariable = false;
};

std::mutex                                            sRegMutex;
std::unordered_map<VkDescriptorSetLayout, LayoutInfo> sLayouts;

bool LookupLayout(VkDescriptorSetLayout l, LayoutInfo& out)
{
    std::lock_guard<std::mutex> lk(sRegMutex);
    auto it = sLayouts.find(l);
    if (it == sLayouts.end()) return false;
    out = it->second;
    return true;
}

// Cost of ONE set of `li` (variableCount applies to the variable binding).
void setCost(const LayoutInfo& li, uint32_t variableCount, VkdTypeCounts& counts, uint32_t& inlineBindings)
{
    for (const Binding& b : li.bindings)
    {
        const uint32_t n = b.variable ? std::min(variableCount, b.count) : b.count;
        counts.add(b.type, n);
        if (b.type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK && n) ++inlineBindings;
    }
}

// ---- device limits ----
struct DeviceLimits
{
    bool                                       valid = false;
    VkPhysicalDeviceLimits                     l{};
    VkPhysicalDeviceVulkan12Properties         v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    bool                                       hasAS = false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
};
DeviceLimits sLimits;
uint32_t sPushCap = 0;            // debug cap (RenderVKPushConstantCap), 0 = off
VkPhysicalDevice sLimitsPd = VK_NULL_HANDLE;

// Error-level report (LL has no non-fatal error level: LL_ERRS aborts). Both the LL
// log and stderr, flagged ERROR so it stands out in a tester's log paste.
void errorBoth(const std::string& msg)
{
    LL_WARNS("Vulkan") << "ERROR: " << msg << LL_ENDL;
    std::fprintf(stderr, "[vktest] ERROR: %s\n", msg.c_str());
    std::fflush(stderr);
}
} // namespace

void vkdSetPushConstantCap(uint32_t bytes)
{
    sPushCap = bytes;
}

uint32_t vkdPushConstantLimit()
{
    if (!sLimits.valid) return 0;
    const uint32_t dev = sLimits.l.maxPushConstantsSize;
    return (sPushCap && sPushCap < dev) ? sPushCap : dev;
}

// ===========================================================================
// device limits
// ===========================================================================
void vkdInitDeviceLimits(VkPhysicalDevice pd)
{
    if (sLimits.valid && sLimitsPd == pd) return;   // once per device (called early + by the renderer)
    sLimitsPd = pd;
    DeviceLimits d;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    if (n) vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) d.hasAS = true;

    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &d.v12;
    if (d.hasAS) d.v12.pNext = &d.as;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    d.l = p2.properties.limits;
    d.v12.pNext = nullptr;
    d.valid = true;
    sLimits = d;

    const VkPhysicalDeviceLimits& l = d.l;
    LL_INFOS("Vulkan") << "descriptor limits (" << p2.properties.deviceName << "): perStage samplers "
        << l.maxPerStageDescriptorSamplers << " sampledImages " << l.maxPerStageDescriptorSampledImages
        << " storageBuffers " << l.maxPerStageDescriptorStorageBuffers << " storageImages "
        << l.maxPerStageDescriptorStorageImages << " resources " << l.maxPerStageResources
        << "; perSet samplers " << l.maxDescriptorSetSamplers << " sampledImages "
        << l.maxDescriptorSetSampledImages << " storageBuffers " << l.maxDescriptorSetStorageBuffers
        << "; UAB perStage sampledImages " << d.v12.maxPerStageDescriptorUpdateAfterBindSampledImages
        << " perSet sampledImages " << d.v12.maxDescriptorSetUpdateAfterBindSampledImages
        << "; pushConstants " << l.maxPushConstantsSize << " B" << LL_ENDL;
    // #238: every push block is designed for the spec minimum (128 B); log the limit
    // the layout checks use (the debug cap simulates a 128 B device on a bigger one).
    LL_INFOS("Vulkan") << "maxPushConstantsSize " << l.maxPushConstantsSize << " B (spec minimum 128)"
        << ((sPushCap && sPushCap < l.maxPushConstantsSize)
                ? " -- RenderVKPushConstantCap: layouts checked against " + std::to_string(sPushCap) + " B"
                : std::string()) << LL_ENDL;
    if (l.maxPushConstantsSize < 128)
        errorBoth("device maxPushConstantsSize " + std::to_string(l.maxPushConstantsSize)
                  + " B is below the Vulkan minimum of 128 B");
}

VkdBindlessLimits vkdBindlessLimits(bool uab)
{
    VkdBindlessLimits r;
    if (!sLimits.valid) return r;
    const VkPhysicalDeviceLimits& l = sLimits.l;
    const VkPhysicalDeviceVulkan12Properties& u = sLimits.v12;
    r.perStageSamplers      = uab ? u.maxPerStageDescriptorUpdateAfterBindSamplers      : l.maxPerStageDescriptorSamplers;
    r.perStageSampledImages = uab ? u.maxPerStageDescriptorUpdateAfterBindSampledImages : l.maxPerStageDescriptorSampledImages;
    r.setSamplers           = uab ? u.maxDescriptorSetUpdateAfterBindSamplers           : l.maxDescriptorSetSamplers;
    r.setSampledImages      = uab ? u.maxDescriptorSetUpdateAfterBindSampledImages      : l.maxDescriptorSetSampledImages;
    r.perStageResources     = uab ? u.maxPerStageUpdateAfterBindResources               : l.maxPerStageResources;
    return r;
}

// ===========================================================================
// layout registry
// ===========================================================================
VkResult vkdCreateSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo* ci,
                            VkDescriptorSetLayout* out, const char* name)
{
    VkResult r = vkCreateDescriptorSetLayout(dev, ci, nullptr, out);
    if (r != VK_SUCCESS) return r;

    LayoutInfo li;
    li.name = name ? name : "?";
    li.uab = (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) != 0;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo* bfi = nullptr;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            bfi = (const VkDescriptorSetLayoutBindingFlagsCreateInfo*)p;
    for (uint32_t i = 0; i < ci->bindingCount; ++i)
    {
        const VkDescriptorSetLayoutBinding& b = ci->pBindings[i];
        const VkDescriptorBindingFlags f = (bfi && i < bfi->bindingCount && bfi->pBindingFlags)
                                           ? bfi->pBindingFlags[i] : 0;
        Binding rb{b.descriptorType, b.descriptorCount, b.stageFlags,
                   (f & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) != 0};
        li.hasVariable |= rb.variable;
        if ((f & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) && !li.uab)
        {
            std::ostringstream s;
            s << "descriptor set layout '" << li.name << "' binding " << b.binding
              << " is UPDATE_AFTER_BIND but the layout lacks UPDATE_AFTER_BIND_POOL_BIT (invalid)";
            warnBoth(s.str());
        }
        li.bindings.push_back(rb);
    }
    std::lock_guard<std::mutex> lk(sRegMutex);
    sLayouts[*out] = std::move(li);
    return VK_SUCCESS;
}

void vkdDestroySetLayout(VkDevice dev, VkDescriptorSetLayout l)
{
    if (!l) return;
    {
        std::lock_guard<std::mutex> lk(sRegMutex);
        sLayouts.erase(l);
    }
    vkDestroyDescriptorSetLayout(dev, l, nullptr);
}

// ===========================================================================
// pipeline-layout limit check
// ===========================================================================
namespace
{
enum Cls { C_SAMPLER, C_SAMPLED, C_STORAGE_IMG, C_UBO, C_UBO_DYN, C_SSBO, C_SSBO_DYN, C_INPUT, C_AS, C_COUNT };

void classify(VkDescriptorType t, uint64_t n, uint64_t* c)
{
    switch (t)
    {
    case VK_DESCRIPTOR_TYPE_SAMPLER:                c[C_SAMPLER] += n; break;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: c[C_SAMPLER] += n; c[C_SAMPLED] += n; break;
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:   c[C_SAMPLED] += n; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:   c[C_STORAGE_IMG] += n; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:         c[C_UBO] += n; break;
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: c[C_UBO] += n; c[C_UBO_DYN] += n; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:         c[C_SSBO] += n; break;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: c[C_SSBO] += n; c[C_SSBO_DYN] += n; break;
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:       c[C_INPUT] += n; break;
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: c[C_AS] += n; break;
    default: break;
    }
}

bool over(const char* pl, const char* what, uint64_t used, uint64_t limit, std::ostringstream& msgs)
{
    if (used <= limit) return false;
    msgs << "\n  " << what << ": " << used << " > limit " << limit << " (over by " << (used - limit) << ")";
    (void)pl;
    return true;
}
} // namespace

VkResult vkdCreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo* ci,
                                 VkPipelineLayout* out, const char* name)
{
    if (sLimits.valid)
    {
        const VkPhysicalDeviceLimits& L = sLimits.l;
        const VkPhysicalDeviceVulkan12Properties& U = sLimits.v12;
        std::ostringstream msgs;
        bool bad = false;

        // Push constants (#238). Past the REAL device limit: invalid usage that crashes
        // some drivers (AMD Polaris, 128 B) -> refuse with an ERROR naming the layout.
        // Past only the debug cap (RenderVKPushConstantCap): ERROR log, still created.
        uint32_t pcEnd = 0;
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; ++i)
            pcEnd = std::max(pcEnd, ci->pPushConstantRanges[i].offset + ci->pPushConstantRanges[i].size);
        const uint32_t pcLimit = vkdPushConstantLimit();
        if (pcEnd > pcLimit)
        {
            const bool real = pcEnd > L.maxPushConstantsSize;
            errorBoth(std::string("pipeline layout '") + (name ? name : "?") + "' push constants "
                      + std::to_string(pcEnd) + " B exceed " + (real ? "the device maxPushConstantsSize "
                                                                     : "RenderVKPushConstantCap ")
                      + std::to_string(real ? L.maxPushConstantsSize : pcLimit) + " B"
                      + (real ? " -- layout NOT created" : " (debug cap: created anyway)"));
            if (real)
            {
                *out = VK_NULL_HANDLE;
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
        bad |= over(name, "bound descriptor sets", ci->setLayoutCount, L.maxBoundDescriptorSets, msgs);

        // Two passes: plain limits count only non-UAB layouts; UAB limits count all
        // (spec 'Limits' chapter), and apply only when any layout is UAB.
        bool anyUab = false;
        for (int pass = 0; pass < 2; ++pass)
        {
            const bool uab = pass == 1;
            uint64_t setTot[C_COUNT] = {};
            // per stage bit
            uint64_t stage[32][C_COUNT] = {};
            for (uint32_t s = 0; s < ci->setLayoutCount; ++s)
            {
                LayoutInfo li;
                if (!ci->pSetLayouts[s] || !LookupLayout(ci->pSetLayouts[s], li)) continue;
                anyUab |= li.uab;
                if (!uab && li.uab) continue;
                for (const Binding& b : li.bindings)
                {
                    classify(b.type, b.count, setTot);
                    for (uint32_t bit = 0; bit < 32; ++bit)
                        if (b.stages & (1u << bit)) classify(b.type, b.count, stage[bit]);
                }
            }
            if (uab && !anyUab) break;

            uint64_t lim_samp   = uab ? U.maxPerStageDescriptorUpdateAfterBindSamplers        : L.maxPerStageDescriptorSamplers;
            uint64_t lim_simg   = uab ? U.maxPerStageDescriptorUpdateAfterBindSampledImages   : L.maxPerStageDescriptorSampledImages;
            uint64_t lim_stimg  = uab ? U.maxPerStageDescriptorUpdateAfterBindStorageImages   : L.maxPerStageDescriptorStorageImages;
            uint64_t lim_ubo    = uab ? U.maxPerStageDescriptorUpdateAfterBindUniformBuffers  : L.maxPerStageDescriptorUniformBuffers;
            uint64_t lim_ssbo   = uab ? U.maxPerStageDescriptorUpdateAfterBindStorageBuffers  : L.maxPerStageDescriptorStorageBuffers;
            uint64_t lim_input  = uab ? U.maxPerStageDescriptorUpdateAfterBindInputAttachments : L.maxPerStageDescriptorInputAttachments;
            uint64_t lim_res    = uab ? U.maxPerStageUpdateAfterBindResources                 : L.maxPerStageResources;
            uint64_t lim_as     = sLimits.hasAS ? (uab ? sLimits.as.maxPerStageDescriptorUpdateAfterBindAccelerationStructures
                                                       : sLimits.as.maxPerStageDescriptorAccelerationStructures) : 0;
            const char* sfx = uab ? " [update-after-bind]" : "";
            for (uint32_t bit = 0; bit < 32; ++bit)
            {
                const uint64_t* c = stage[bit];
                const uint64_t res = c[C_SAMPLED] + c[C_STORAGE_IMG] + c[C_UBO] + c[C_SSBO] + c[C_INPUT];
                if (!res && !c[C_SAMPLER] && !c[C_AS]) continue;
                std::string p = "stage 0x" + [&]{ std::ostringstream h; h << std::hex << (1u << bit); return h.str(); }() + sfx + " ";
                bad |= over(name, (p + "samplers").c_str(), c[C_SAMPLER], lim_samp, msgs);
                bad |= over(name, (p + "sampled images").c_str(), c[C_SAMPLED], lim_simg, msgs);
                bad |= over(name, (p + "storage images").c_str(), c[C_STORAGE_IMG], lim_stimg, msgs);
                bad |= over(name, (p + "uniform buffers").c_str(), c[C_UBO], lim_ubo, msgs);
                bad |= over(name, (p + "storage buffers").c_str(), c[C_SSBO], lim_ssbo, msgs);
                bad |= over(name, (p + "input attachments").c_str(), c[C_INPUT], lim_input, msgs);
                bad |= over(name, (p + "resources").c_str(), res, lim_res, msgs);
                if (c[C_AS]) bad |= over(name, (p + "acceleration structures").c_str(), c[C_AS], lim_as, msgs);
            }
            std::string p = std::string("layout total") + sfx + " ";
            bad |= over(name, (p + "samplers").c_str(), setTot[C_SAMPLER],
                        uab ? U.maxDescriptorSetUpdateAfterBindSamplers : L.maxDescriptorSetSamplers, msgs);
            bad |= over(name, (p + "sampled images").c_str(), setTot[C_SAMPLED],
                        uab ? U.maxDescriptorSetUpdateAfterBindSampledImages : L.maxDescriptorSetSampledImages, msgs);
            bad |= over(name, (p + "storage images").c_str(), setTot[C_STORAGE_IMG],
                        uab ? U.maxDescriptorSetUpdateAfterBindStorageImages : L.maxDescriptorSetStorageImages, msgs);
            bad |= over(name, (p + "uniform buffers").c_str(), setTot[C_UBO],
                        uab ? U.maxDescriptorSetUpdateAfterBindUniformBuffers : L.maxDescriptorSetUniformBuffers, msgs);
            bad |= over(name, (p + "dynamic uniform buffers").c_str(), setTot[C_UBO_DYN],
                        uab ? U.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic : L.maxDescriptorSetUniformBuffersDynamic, msgs);
            bad |= over(name, (p + "storage buffers").c_str(), setTot[C_SSBO],
                        uab ? U.maxDescriptorSetUpdateAfterBindStorageBuffers : L.maxDescriptorSetStorageBuffers, msgs);
            bad |= over(name, (p + "dynamic storage buffers").c_str(), setTot[C_SSBO_DYN],
                        uab ? U.maxDescriptorSetUpdateAfterBindStorageBuffersDynamic : L.maxDescriptorSetStorageBuffersDynamic, msgs);
            bad |= over(name, (p + "input attachments").c_str(), setTot[C_INPUT],
                        uab ? U.maxDescriptorSetUpdateAfterBindInputAttachments : L.maxDescriptorSetInputAttachments, msgs);
            if (setTot[C_AS])
                bad |= over(name, (p + "acceleration structures").c_str(), setTot[C_AS],
                            sLimits.hasAS ? (uab ? sLimits.as.maxDescriptorSetUpdateAfterBindAccelerationStructures
                                                 : sLimits.as.maxDescriptorSetAccelerationStructures) : 0, msgs);
        }
        if (bad)
            warnBoth(std::string("pipeline layout '") + (name ? name : "?")
                     + "' exceeds device descriptor/push limits:" + msgs.str());
    }
    return vkCreatePipelineLayout(dev, ci, nullptr, out);
}

// ===========================================================================
// pool
// ===========================================================================
VkResult VkDescPool::create(VkDevice dev, const char* name, std::initializer_list<VkdPoolUse> uses,
                            VkDescriptorPoolCreateFlags extraFlags)
{
    return create(dev, name, std::vector<VkdPoolUse>(uses), extraFlags);
}

VkResult VkDescPool::create(VkDevice dev, const char* name, const std::vector<VkdPoolUse>& uses,
                            VkDescriptorPoolCreateFlags extraFlags)
{
    if (mPool) destroy();
    mDev = dev;
    mName = name ? name : "?";
    mCap.clear(); mUsed.clear(); mLive.clear();
    mMaxSets = mUsedSets = 0;
    mInlineBindCap = mInlineBindUsed = 0;
    mFlags = extraFlags;

    std::ostringstream desc;
    for (const VkdPoolUse& u : uses)
    {
        LayoutInfo li;
        if (!u.layout || !LookupLayout(u.layout, li))
        {
            warnBoth(std::string("descriptor pool '") + mName
                     + "': a layout was not created through vkdCreateSetLayout; cannot size it");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkdTypeCounts one;
        uint32_t inl = 0;
        setCost(li, u.variableCount, one, inl);
        for (const auto& p : one.v) mCap.add(p.first, p.second * u.sets);
        mInlineBindCap += inl * u.sets;
        mMaxSets += u.sets;
        if (li.uab) mFlags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        desc << " " << li.name << "x" << u.sets;
    }

    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto& p : mCap.v)
    {
        if (p.second > UINT32_MAX)
        {
            warnBoth(std::string("descriptor pool '") + mName + "': " + typeName(p.first)
                     + " count overflows uint32");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        sizes.push_back({p.first, (uint32_t)p.second});
    }

    VkDescriptorPoolInlineUniformBlockCreateInfo inl{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO};
    inl.maxInlineUniformBlockBindings = mInlineBindCap;
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.pNext = mInlineBindCap ? &inl : nullptr;
    pci.flags = mFlags;
    pci.maxSets = std::max(1u, mMaxSets);
    pci.poolSizeCount = (uint32_t)sizes.size();
    pci.pPoolSizes = sizes.empty() ? nullptr : sizes.data();
    VkResult r = vkCreateDescriptorPool(dev, &pci, nullptr, &mPool);
    std::ostringstream s;
    s << "descriptor pool '" << mName << "': maxSets " << pci.maxSets << " from" << desc.str() << ";";
    for (const auto& p : sizes) s << " " << typeName(p.type) << "=" << p.descriptorCount;
    if (mFlags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) s << " [UAB]";
    if (mFlags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) s << " [FREE]";
    if (r != VK_SUCCESS)
    {
        s << " -> vkCreateDescriptorPool failed (" << vkResultString(r) << ")";
        warnBoth(s.str());
        mPool = VK_NULL_HANDLE;
        return r;
    }
    LL_INFOS("Vulkan") << s.str() << LL_ENDL;
    return VK_SUCCESS;
}

VkResult VkDescPool::allocate(VkDescriptorSetLayout layout, VkDescriptorSet* out, uint32_t variableCount)
{
    return allocChecked(1, &layout, out, &variableCount);
}

VkResult VkDescPool::allocate(uint32_t n, const VkDescriptorSetLayout* layouts, VkDescriptorSet* out,
                              const uint32_t* variableCounts)
{
    return allocChecked(n, layouts, out, variableCounts);
}

VkResult VkDescPool::allocChecked(uint32_t n, const VkDescriptorSetLayout* layouts, VkDescriptorSet* out,
                                  const uint32_t* variableCounts)
{
    if (!mPool)
    {
        warnBoth(std::string("descriptor pool '") + mName + "': allocate on a pool that was never created");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<SetCost> costs(n);
    std::vector<uint32_t> vcounts(n, 0);
    VkdTypeCounts total;
    uint32_t totalInline = 0;
    bool anyVariable = false;
    std::vector<const char*> names(n, "?");
    for (uint32_t i = 0; i < n; ++i)
    {
        LayoutInfo li;
        if (!LookupLayout(layouts[i], li))
        {
            warnBoth(std::string("descriptor pool '") + mName
                     + "': allocating a layout not created through vkdCreateSetLayout (unaccounted)");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        names[i] = li.name;
        const uint32_t vc = variableCounts ? variableCounts[i] : UINT32_MAX;
        if (li.hasVariable)
        {
            anyVariable = true;
            for (const Binding& b : li.bindings)
                if (b.variable)
                {
                    if (vc != UINT32_MAX && vc > b.count)
                    {
                        std::ostringstream s;
                        s << "descriptor pool '" << mName << "': layout '" << li.name
                          << "' variable count " << vc << " exceeds its binding maximum " << b.count;
                        warnBoth(s.str());
                        return VK_ERROR_OUT_OF_POOL_MEMORY;
                    }
                    vcounts[i] = std::min(vc, b.count);
                }
        }
        setCost(li, vc, costs[i].counts, costs[i].inlineBindings);
        for (const auto& p : costs[i].counts.v) total.add(p.first, p.second);
        totalInline += costs[i].inlineBindings;
    }

    // ---- accounting check (all drivers) ----
    bool fail = false;
    std::ostringstream s;
    s << "descriptor pool '" << mName << "' exhausted allocating [";
    for (uint32_t i = 0; i < n; ++i) s << (i ? ", " : "") << names[i];
    s << "]:";
    if (mUsedSets + n > mMaxSets)
    {
        fail = true;
        s << "\n  sets: need " << n << ", " << (mMaxSets - mUsedSets) << " left of " << mMaxSets
          << " (short by " << (mUsedSets + n - mMaxSets) << ")";
    }
    for (const auto& p : total.v)
    {
        const uint64_t cap = mCap.get(p.first), used = mUsed.get(p.first);
        const uint64_t left = cap > used ? cap - used : 0;
        if (p.second > left)
        {
            fail = true;
            s << "\n  " << typeName(p.first) << ": need " << p.second << ", " << left << " left of " << cap
              << " (short by " << (p.second - left) << ")";
        }
    }
    if (mInlineBindUsed + totalInline > mInlineBindCap)
    {
        fail = true;
        s << "\n  inline uniform block bindings: short by " << (mInlineBindUsed + totalInline - mInlineBindCap);
    }
    if (fail)
    {
        s << "\n  (pool is sized from its declared layouts: a set allocated from it is missing from"
             " its VkDescPool::create list, or a set is allocated more times than declared)";
        warnBoth(s.str());
        return VK_ERROR_OUT_OF_POOL_MEMORY;   // what a strict driver (RADV) returns
    }

    VkDescriptorSetVariableDescriptorCountAllocateInfo vai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
    vai.descriptorSetCount = n;
    vai.pDescriptorCounts = vcounts.data();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.pNext = anyVariable ? &vai : nullptr;
    ai.descriptorPool = mPool;
    ai.descriptorSetCount = n;
    ai.pSetLayouts = layouts;
    VkResult r = vkAllocateDescriptorSets(mDev, &ai, out);
    if (r != VK_SUCCESS)
    {
        std::ostringstream e;
        e << "descriptor pool '" << mName << "': driver rejected an allocation the accounting allowed ("
          << vkResultString(r) << "; " << (mUsedSets) << "/" << mMaxSets << " sets used"
          << ((mFlags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) ? ", pool may be fragmented" : "")
          << ")";
        warnBoth(e.str());
        return r;
    }
    mUsedSets += n;
    for (const auto& p : total.v) mUsed.add(p.first, p.second);
    mInlineBindUsed += totalInline;
    if (mFlags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        for (uint32_t i = 0; i < n; ++i) mLive[out[i]] = std::move(costs[i]);
    return VK_SUCCESS;
}

void VkDescPool::free(uint32_t n, const VkDescriptorSet* sets)
{
    if (!mPool || !n) return;
    if (!(mFlags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT))
    {
        warnBoth(std::string("descriptor pool '") + mName
                 + "': free() on a pool without FREE_DESCRIPTOR_SET_BIT (invalid; ignored)");
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        auto it = mLive.find(sets[i]);
        if (it == mLive.end()) continue;   // VK_NULL_HANDLE or foreign: nothing to credit
        for (const auto& p : it->second.counts.v) mUsed.sub(p.first, p.second);
        mInlineBindUsed -= std::min(mInlineBindUsed, it->second.inlineBindings);
        if (mUsedSets) --mUsedSets;
        mLive.erase(it);
    }
    vkFreeDescriptorSets(mDev, mPool, n, sets);
}

void VkDescPool::reset()
{
    if (!mPool) return;
    vkResetDescriptorPool(mDev, mPool, 0);
    mUsed.clear();
    mUsedSets = 0;
    mInlineBindUsed = 0;
    mLive.clear();
}

void VkDescPool::destroy()
{
    if (mPool && mDev) vkDestroyDescriptorPool(mDev, mPool, nullptr);
    mPool = VK_NULL_HANDLE;
    mCap.clear(); mUsed.clear(); mLive.clear();
    mMaxSets = mUsedSets = 0;
    mInlineBindCap = mInlineBindUsed = 0;
}
