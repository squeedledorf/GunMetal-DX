// vk_descpool.h -- descriptor pools sized from the layouts actually allocated from
// them, with allocation accounting that is enforced on EVERY driver.
//
// Why: the VK renderer's pools used to be sized with hand-maintained magic counts.
// NVIDIA does not enforce VkDescriptorPoolSize / maxSets, so stale counts went
// unnoticed; Mesa RADV enforces them exactly and returned
// VK_ERROR_OUT_OF_POOL_MEMORY right after login. This helper removes the magic:
//
//   1. Layout registry. Create every pool-allocated set layout through
//      vkdCreateSetLayout(); it records each binding's type and descriptorCount,
//      the variable-count binding (VARIABLE_DESCRIPTOR_COUNT_BIT, bindless) and
//      whether the layout needs an UPDATE_AFTER_BIND pool.
//   2. VkDescPool::create(name, {{layout, setCount[, variableCount]}, ...})
//      derives VkDescriptorPoolSize[] + maxSets (+ the inline-uniform-block pool
//      struct, + UPDATE_AFTER_BIND_BIT when any layout requires it) from those
//      layouts. No hand counts anywhere.
//   3. VkDescPool::allocate() checks the request against the pool's remaining
//      per-type counts and sets BEFORE calling the driver. A request that would
//      not fit logs LL_WARNS (pool, layout, type, shortfall) and returns
//      VK_ERROR_OUT_OF_POOL_MEMORY without calling the driver -- i.e. it fails
//      exactly the way RADV would, on NVIDIA too.
//   4. FREE_DESCRIPTOR_SET_BIT pools credit freed sets back (per-set cost is
//      remembered); reset() clears everything (vkResetDescriptorPool).
//
// Also: vkdCreatePipelineLayout() checks the combined per-stage / per-layout
// descriptor limits (plain or update-after-bind flavour) and push-constant size
// against the device, logging anything that exceeds them (RADV fails pipeline
// creation or misbehaves where NVIDIA's huge limits hide it).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// #697: a descriptor-set allocation on a LAZY (feature enable / re-create) path that answers
// out of memory -- or SLVkInjectOomCalls -- runs `onfail` instead of aborting: the caller
// tears its partial state down and returns false (the feature stays off this call and is
// retried on a later one). Any other error stays fatal (VK_CHECK). Needs vk_common.h.
#define VKD_ALLOC_SOFT(expr, onfail)                                                   \
    do {                                                                               \
        const VkResult _vkd_r = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY : (expr); \
        if (vkIsOom(_vkd_r)) { vkLowMemNote(VKLM_DESCRIPTOR, false); onfail; }         \
        VK_CHECK(_vkd_r);                                                              \
    } while (0)

// Per-type descriptor counts. Small linear map: a handful of types per layout.
struct VkdTypeCounts
{
    std::vector<std::pair<VkDescriptorType, uint64_t>> v;
    uint64_t get(VkDescriptorType t) const;
    void add(VkDescriptorType t, uint64_t n);
    void sub(VkDescriptorType t, uint64_t n);
    void clear() { v.clear(); }
};

// ---- device limits (set once per device, used by the checks) ----
// Queries VkPhysicalDeviceLimits + Vulkan 1.2 update-after-bind limits (+ AS limits
// when VK_KHR_acceleration_structure is supported). Logs the relevant limits once.
void vkdInitDeviceLimits(VkPhysicalDevice pd);
// DEBUG (#238, RenderVKPushConstantCap): check push-constant ranges against
// min(device limit, bytes) instead of the device limit, so a big-push device (NVIDIA
// 256 B) reports what would break on a 128 B one (AMD Polaris). 0 = off. Set before
// vkdInitDeviceLimits. Check-only: layouts are still created on the real device.
void vkdSetPushConstantCap(uint32_t bytes);
// The push-constant limit the checks use (device limit, or the debug cap if lower).
uint32_t vkdPushConstantLimit();

struct VkdBindlessLimits
{
    uint32_t perStageSamplers = 0, perStageSampledImages = 0;
    uint32_t setSamplers = 0, setSampledImages = 0;
    uint32_t perStageResources = 0;
};
// Limits that bound a bindless COMBINED_IMAGE_SAMPLER array (plain or UAB flavour).
VkdBindlessLimits vkdBindlessLimits(bool updateAfterBind);

// ---- layout registry ----
// Creates the layout and records its bindings. `name` must outlive the program
// (string literal). Registration overwrites any stale record for a reused handle.
VkResult vkdCreateSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo* ci,
                            VkDescriptorSetLayout* out, const char* name);
// Destroys the layout and forgets its record.
void vkdDestroySetLayout(VkDevice dev, VkDescriptorSetLayout l);

// Creates the pipeline layout after checking its combined descriptor + push-constant
// usage against the device limits (logged; creation proceeds either way so the
// driver's own verdict is also visible) -- EXCEPT a push-constant range past the real
// device maxPushConstantsSize: invalid usage that crashes some drivers (#238, AMD
// Polaris at 128 B), so that logs an ERROR naming the layout and returns
// VK_ERROR_INITIALIZATION_FAILED without calling the driver.
VkResult vkdCreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo* ci,
                                 VkPipelineLayout* out, const char* name);

// ---- pool ----
struct VkdPoolUse
{
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    uint32_t sets = 1;
    // For a layout with a variable-count binding: the count each set allocates with.
    // UINT32_MAX = the binding's declared (maximum) descriptorCount.
    uint32_t variableCount = UINT32_MAX;
};

class VkDescPool
{
public:
    VkDescPool() = default;
    VkDescPool(const VkDescPool&) = delete;
    VkDescPool& operator=(const VkDescPool&) = delete;
    ~VkDescPool() = default;   // destroy() is explicit (needs the device alive)

    // Size the pool from `uses`. `extraFlags` may add FREE_DESCRIPTOR_SET_BIT;
    // UPDATE_AFTER_BIND_BIT is added automatically when any layout needs it.
    VkResult create(VkDevice dev, const char* name, std::initializer_list<VkdPoolUse> uses,
                    VkDescriptorPoolCreateFlags extraFlags = 0);
    // Same, from a runtime list.
    VkResult create(VkDevice dev, const char* name, const std::vector<VkdPoolUse>& uses,
                    VkDescriptorPoolCreateFlags extraFlags = 0);

    // Accounted allocation. variableCount applies to a layout's variable-count
    // binding (UINT32_MAX = its declared maximum); ignored for other layouts.
    VkResult allocate(VkDescriptorSetLayout layout, VkDescriptorSet* out,
                      uint32_t variableCount = UINT32_MAX);
    // N sets in one call (mixed layouts). The whole request is checked first.
    VkResult allocate(uint32_t n, const VkDescriptorSetLayout* layouts, VkDescriptorSet* out,
                      const uint32_t* variableCounts = nullptr);

    // Individual free (FREE_DESCRIPTOR_SET_BIT pools only; logged otherwise).
    void free(uint32_t n, const VkDescriptorSet* sets);
    // vkResetDescriptorPool: every set returns to the pool.
    void reset();
    void destroy();

    VkDescriptorPool handle() const { return mPool; }
    operator VkDescriptorPool() const { return mPool; }
    const char* name() const { return mName; }

private:
    VkResult allocChecked(uint32_t n, const VkDescriptorSetLayout* layouts, VkDescriptorSet* out,
                          const uint32_t* variableCounts);

    VkDevice                    mDev = VK_NULL_HANDLE;
    VkDescriptorPool            mPool = VK_NULL_HANDLE;
    const char*                 mName = "";
    VkDescriptorPoolCreateFlags mFlags = 0;
    VkdTypeCounts               mCap, mUsed;
    uint32_t                    mMaxSets = 0, mUsedSets = 0;
    uint32_t                    mInlineBindCap = 0, mInlineBindUsed = 0;
    // FREE_DESCRIPTOR_SET_BIT pools: per-set cost, credited back on free().
    struct SetCost { VkdTypeCounts counts; uint32_t inlineBindings = 0; };
    std::unordered_map<VkDescriptorSet, SetCost> mLive;
};
