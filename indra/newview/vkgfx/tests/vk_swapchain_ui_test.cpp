// Standalone WSI protocol tests: no display, Vulkan driver, or viewer process.
// Link this file with ../vk_swapchain.cpp; every Vulkan entry used is a mock.
// Give the mocked device a non-null sentinel. No production test hooks or Vulkan
// device are created; include library headers before exposing only this fixture.
#include "../vk_common.h"
#include "../vk_requirements.h"
#include <map>
#include <functional>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#define private public
#include "../vk_device.h"
#undef private
#include "../vk_swapchain.h"
#include "../vk_retirement_retry.h"
#include <cassert>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>

namespace
{
uintptr_t nextHandle = 1;
template<class T> T handle() { return reinterpret_cast<T>(nextHandle++); }
VkResult acquireResult = VK_SUCCESS;
VkResult presentResult = VK_SUCCESS;
bool failView = false, failFenceOnce = false;
unsigned resets = 0, lostCalls = 0, deviceWaits = 0;
bool completeAcquire = true, completePresent = true, failReset = false;
std::unordered_map<VkFence, bool> signaled;
VkFence lastAcquire = VK_NULL_HANDLE, lastPresent = VK_NULL_HANDLE;
uint64_t acquireTimeout = UINT64_MAX;
VkPresentModeKHR createdMode = VK_PRESENT_MODE_FIFO_KHR;
VkImageUsageFlags createdUsage = 0;
VkSemaphore waitedPresent = VK_NULL_HANDLE;
std::unordered_set<VkSemaphore> semaphores;
std::unordered_set<VkFence> fences;
std::unordered_set<VkImageView> views;
std::unordered_set<VkSwapchainKHR> chains;
}

bool vkDeviceIsLost() { return false; }
bool vkDeviceLostSimulated() { return false; }
bool vkSimulateDeviceLostTake(int) { return false; }
void vkDeviceLostRaise(const char*, VkResult, const char*, const char*, int) { ++lostCalls; }
const char* vkResultString(VkResult) { return "mock Vulkan result"; }
void vkAddrFrameTick() {}
void (*gVkDeviceLostHandler)(const char*, VkResult, const char*, const char*, int) = nullptr;
void (*gVkFatalHandler)(const char*, VkResult, const char*, int) = nullptr;
VkResult vkqDeviceWaitIdle(VkDevice, const char*, int) { ++deviceWaits; return VK_SUCCESS; }
VkResult vkqWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t, const char*, int) { return VK_SUCCESS; }
VkResult vkqGetFenceStatus(VkDevice, VkFence fence, const char*, int) { return signaled[fence] ? VK_SUCCESS : VK_NOT_READY; }
VkResult vkqCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* out)
{
    if (failView) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = handle<VkImageView>(); views.insert(*out); return VK_SUCCESS;
}
void vkqDestroyImageView(VkDevice, VkImageView view, const VkAllocationCallbacks*) { assert(views.erase(view) == 1); }
void vkCompatGetDeviceImageMemoryRequirements(VkDevice, const VkDeviceImageMemoryRequirements*, VkMemoryRequirements2* out)
{ out->memoryRequirements.size = 4096; }

extern "C"
{
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32* out)
{ *out = VK_TRUE; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* out)
{
    *out = {};
    out->minImageCount = 2;
    out->maxImageCount = 3;
    out->currentExtent = {800, 600};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkSurfaceFormatKHR* out)
{
    if (out) out[0] = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    *count = 1; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkPresentModeKHR* out)
{
    if (out) { out[0] = VK_PRESENT_MODE_FIFO_KHR; out[1] = VK_PRESENT_MODE_MAILBOX_KHR; }
    *count = 2; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR* out)
{
    createdMode = info->presentMode;
    createdUsage = info->imageUsage;
    *out = handle<VkSwapchainKHR>(); chains.insert(*out); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR chain, const VkAllocationCallbacks*)
{ assert(chains.erase(chain) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* out)
{
    if (out) for (unsigned i = 0; i < 3; ++i) out[i] = handle<VkImage>();
    *count = 3; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out)
{ *out = handle<VkSemaphore>(); semaphores.insert(*out); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore semaphore, const VkAllocationCallbacks*)
{ assert(semaphores.erase(semaphore) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo* info, const VkAllocationCallbacks*, VkFence* out)
{
    if (failFenceOnce) { failFenceOnce = false; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    *out = handle<VkFence>(); fences.insert(*out);
    signaled[*out] = bool(info->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*)
{ assert(fences.erase(fence) == 1); }
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t count, const VkFence* values)
{ ++resets; if (failReset) return VK_ERROR_OUT_OF_DEVICE_MEMORY; for (uint32_t i = 0; i < count; ++i) signaled[values[i]] = false; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t timeout, VkSemaphore, VkFence fence, uint32_t* out)
{
    acquireTimeout = timeout; *out = 2; lastAcquire = fence;
    if (fence && (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)) signaled[fence] = completeAcquire;
    return acquireResult;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR* info)
{
    assert(info->waitSemaphoreCount == 1 && info->pImageIndices[0] == 2);
    waitedPresent = info->pWaitSemaphores[0];
    if (info->pNext)
    {
        const auto* proof = static_cast<const VkSwapchainPresentFenceInfoEXT*>(info->pNext);
        assert(proof->swapchainCount == 1);
        lastPresent = proof->pFences[0];
        if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR ||
            presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_ERROR_SURFACE_LOST_KHR)
            signaled[lastPresent] = completePresent;
    }
    return presentResult;
}
}

int main()
{
    // A permanent queue OOM must not turn a 1000 Hz main loop into 1000 idle
    // waits per second. Only three attempts are admitted; success resets policy.
    VkRetirementRetry retry;
    unsigned attempts = 0;
    const auto epoch = VkRetirementRetry::Clock::time_point{};
    for (unsigned tick = 0; tick < 10000; ++tick)
    {
        const auto now = epoch + std::chrono::milliseconds(tick);
        if (retry.ready(now)) { ++attempts; retry.refused(now); }
    }
    assert(attempts == 3 && retry.exhausted());
    retry.succeeded();
    assert(retry.ready(epoch));
    VkDeviceContext device;
    device.mDevice = handle<VkDevice>();
    device.mPhysical = handle<VkPhysicalDevice>();
    device.mQueue = handle<VkQueue>();
    VkSwapchain primary, auxiliary;
    VkSwapchain::sVSync = true;
    const VkSurfaceKHR surface = handle<VkSurfaceKHR>();
    assert(primary.init(device, surface, 800, 600));
    assert(createdMode == VK_PRESENT_MODE_FIFO_KHR);
    auxiliary.setUIOnly();
    assert(auxiliary.init(device, surface, 800, 600));
    assert(createdMode == VK_PRESENT_MODE_MAILBOX_KHR);
    assert(createdUsage == VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    assert(VkSwapchain::sVSync); // auxiliary policy never changes the main setting
    assert(primary.presentMode() == VK_PRESENT_MODE_FIFO_KHR);
    assert(auxiliary.imageCount() == 3 && auxiliary.framesInFlight() == 2);
    assert(auxiliary.memoryBytes() == 3 * 4096);
    assert(auxiliary.renderFinishedSemaphore(0) != auxiliary.renderFinishedSemaphore(2));
    assert(auxiliary.slotDone(handle<VkDevice>(), 0));
    uint32_t image = 0;
    bool recreate = false;
    acquireResult = VK_TIMEOUT;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(acquireTimeout == 0 && resets == 1 && !recreate);
    acquireResult = VK_ERROR_OUT_OF_DATE_KHR;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(recreate && resets == 2);
    acquireResult = VK_SUBOPTIMAL_KHR;
    assert(auxiliary.acquire(device, &image, &recreate));
    assert(recreate && resets == 4 && image == 2); // delivered image MUST be presented
    assert(auxiliary.present(device, image, &recreate));
    assert(waitedPresent == auxiliary.renderFinishedSemaphore(2));
    assert(auxiliary.frameSlot() == 1);
    acquireResult = VK_ERROR_SURFACE_LOST_KHR;
    assert(!auxiliary.acquire(device, &image, &recreate));
    assert(auxiliary.failed() && lostCalls == 0); // never turns an auxiliary surface loss into app loss
    assert(!auxiliary.retired(device.device())); // fallback present still needs the retirement batch
    const unsigned beforeRetire = deviceWaits;
    auxiliary.presentingQueueIdle(); // owner has performed the one batched presenting-queue wait
    assert(auxiliary.retired(device.device()));
    auxiliary.destroy(device);
    assert(deviceWaits == beforeRetire); // aux destruction itself NEVER waits device idle
    primary.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Allocation failure during view creation returns control and partial
    // teardown releases the chain, with no fatal/abort or leaked sync objects.
    VkSwapchain refused;
    refused.setUIOnly(); failView = true;
    assert(!refused.init(device, surface, 800, 600));
    refused.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Present-fence fast path: stop resize/close until both GPU and PE proofs
    // complete. No device wait, no successor chain, reset-but-unsubmitted fence
    // does not prevent destruction of an abandoned acquisition.
    failView = false;
    VkSwapchain fast;
    fast.setUIOnly(true);
    assert(fast.init(device, surface, 800, 600));
    acquireResult = VK_SUCCESS; presentResult = VK_SUCCESS;
    completePresent = false;
    assert(fast.acquire(device, &image, &recreate));
    const VkFence gpu = fast.inFlightFence();
    fast.submitted();
    assert(fast.present(device, image, &recreate));
    const unsigned fastWaits = deviceWaits;
    assert(!fast.retired(device.device()));
    assert(!fast.recreate(device, 900, 700));
    signaled[gpu] = true;
    assert(!fast.retired(device.device()));
    signaled[lastPresent] = true;
    assert(fast.retired(device.device()));
    assert(fast.recreate(device, 900, 700));
    assert(deviceWaits == fastWaits);
    completeAcquire = false;
    assert(fast.acquire(device, &image, &recreate));
    assert(!fast.retired(device.device())); // acquire signal is not queue work
    signaled[lastAcquire] = true;
    assert(fast.retired(device.device())); // no accepted GPU submit for reset slot
    fast.destroy(device);
    assert(deviceWaits == fastWaits);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    VkSwapchain resetRefused;
    resetRefused.setUIOnly(true);
    assert(resetRefused.init(device, surface, 800, 600));
    completeAcquire = true;
    assert(resetRefused.acquire(device, &image, &recreate));
    const VkFence resetGpu = resetRefused.inFlightFence();
    resetRefused.submitted();
    failReset = true;
    assert(!resetRefused.present(device, image, &recreate));
    assert(resetRefused.failed()); // no future acquire can silently exhaust images
    assert(!resetRefused.retired(device.device()));
    signaled[resetGpu] = true;
    assert(resetRefused.retired(device.device())); // no present was accepted
    failReset = false;
    resetRefused.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());

    // Optional present proofs must not turn main-window allocation refusal into
    // VK_CHECK abort. Keep its usable chain on the existing teardown policy.
    device.mSwapchainMaintenance = true;
    failFenceOnce = true;
    VkSwapchain mainRefusedProof;
    assert(mainRefusedProof.init(device, surface, 800, 600));
    assert(mainRefusedProof.valid());
    mainRefusedProof.destroy(device);
    assert(semaphores.empty() && fences.empty() && views.empty() && chains.empty());
    device.mSwapchainMaintenance = false;

    VkSwapchain ambiguous;
    ambiguous.setUIOnly(true);
    assert(ambiguous.init(device, surface, 800, 600));
    assert(ambiguous.acquire(device, &image, &recreate));
    signaled[ambiguous.inFlightFence()] = true;
    ambiguous.submitted();
    presentResult = VK_ERROR_UNKNOWN;
    assert(!ambiguous.present(device, image, &recreate));
    assert(ambiguous.failed() && !ambiguous.retired(device.device()));
    // Unresolved PE ownership intentionally remains bounded until process exit.
}
