#include "vk_swapchain.h"

#include <algorithm>
#include <cstdio>

bool VkSwapchain::init(VkDeviceContext& ctx, VkSurfaceKHR surface,
                       uint32_t width, uint32_t height, uint32_t framesInFlight) {
    mSurface = surface;
    mFramesInFlight = framesInFlight ? framesInFlight : 2;

    // Sanity: confirm the graphics queue family can present to this surface.
    VkBool32 presentSupported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queueFamily(),
                                         mSurface, &presentSupported);
    if (!presentSupported) {
        std::fprintf(stderr,
            "[vkgfx] queue family %u does not support present to surface\n",
            ctx.queueFamily());
        return false;
    }

    // createSwapchainObjects() also (re)creates the per-IMAGE render-finished
    // semaphores, since the image count is only known there and can change on
    // recreate().
    if (!createSwapchainObjects(ctx, width, height)) return false;

    // Per-frame-in-flight sync objects: image-available semaphore + in-flight
    // fence. (render-finished is per-image; owned by createSwapchainObjects.)
    mImageAvailable.resize(mFramesInFlight);
    mInFlight.resize(mFramesInFlight);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first acquire doesn't hang
    for (uint32_t i = 0; i < mFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &sci, nullptr, &mImageAvailable[i]));
        VK_CHECK(vkCreateFence(ctx.device(), &fci, nullptr, &mInFlight[i]));
    }
    return true;
}

bool VkSwapchain::sVSync = true;
uint32_t VkSwapchain::sNoVsyncMode = 0;

bool VkSwapchain::createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                         uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        ctx.physicalDevice(), mSurface, &caps));

    // Choose a format: prefer BGRA8 SRGB, else the first available.
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                         &fmtCount, nullptr);
    if (fmtCount == 0) {
        std::fprintf(stderr, "[vkgfx] surface has no formats\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                         &fmtCount, fmts.data());
    mFormat = fmts[0];
    for (const auto& f : fmts) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            mFormat = f; break;
        }
    }

    // Present mode: FIFO is always available (vsync). With vsync off take MAILBOX
    // (latest frame wins, no tearing), else IMMEDIATE (may tear), else FIFO; with
    // sNoVsyncMode 1 the order of the first two flips (IMMEDIATE bypasses a compositor
    // that releases MAILBOX images on its own cadence).
    mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!sVSync)
    {
        std::vector<VkPresentModeKHR> pms;
        // #919: with a full-screen-exclusive chain, the modes valid for it.
        if (!(mCreateChain && mPresentModeQuery && mPresentModeQuery(pms)))
        {
            pms.clear();
            uint32_t pmCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, nullptr);
            pms.resize(pmCount);
            if (pmCount)
                vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, pms.data());
            pms.resize(pmCount);
        }
        bool mailbox = false, immediate = false;
        for (VkPresentModeKHR m : pms)
        {
            mailbox   |= (m == VK_PRESENT_MODE_MAILBOX_KHR);
            immediate |= (m == VK_PRESENT_MODE_IMMEDIATE_KHR);
        }
        if (sNoVsyncMode == 1)
        {
            if (immediate)    mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            else if (mailbox) mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
        else if (mailbox)   mPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (immediate) mPresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }

    // Extent.
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        mExtent = caps.currentExtent;
    } else {
        mExtent.width  = std::clamp(width,  caps.minImageExtent.width,
                                    caps.maxImageExtent.width);
        mExtent.height = std::clamp(height, caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }
    if (mExtent.width == 0 || mExtent.height == 0) {
        // Minimized; caller should skip. Report benign.
        return false;
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.pNext = mCreateChain;   // #919: full-screen exclusive info (Windows), else nullptr
    ci.surface = mSurface;
    ci.minImageCount = imageCount;
    ci.imageFormat = mFormat.format;
    ci.imageColorSpace = mFormat.colorSpace;
    ci.imageExtent = mExtent;
    ci.imageArrayLayers = 1;
    // COLOR_ATTACHMENT for rendering; TRANSFER_DST so a compute/blit path can
    // also copy into the swapchain image (the clean pipeline blits its resolved
    // color target). Both are near-universally supported.
    // TRANSFER_SRC: the world renderer copies the lit scene out for next frame's
    // screen-space reflections.
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single queue family
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mPresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VkResult cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    mChainRejected = false;
    if (cr != VK_SUCCESS && ci.pNext && cr != VK_ERROR_DEVICE_LOST) {
        // #919: a driver refusing the full-screen exclusive swapchain must not take the
        // viewer down: fall back to a plain (composited) swapchain and report it to the owner.
        std::fprintf(stderr, "[vkgfx] vkCreateSwapchainKHR with the full-screen exclusive chain = %s; "
                     "retrying without it (FIFO)\n", vkResultString(cr));
        mChainRejected = true;
        mSwapchain = VK_NULL_HANDLE;
        ci.pNext = nullptr;
        mPresentMode = VK_PRESENT_MODE_FIFO_KHR;   // (the mode came from the exclusive query)
        ci.presentMode = mPresentMode;
        cr = vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &mSwapchain);
    }
    VK_CHECK(cr);

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, nullptr);
    mImages.resize(n);
    vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, mImages.data());

    {   // #407: what these images take in device memory. The same create info the WSI
        // uses for them (no VkImageSwapchainCreateInfoKHR: not allowed in this query).
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = ci.imageFormat;
        ici.extent = {mExtent.width, mExtent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = ci.imageArrayLayers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = ci.imageUsage;
        ici.sharingMode = ci.imageSharingMode;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkDeviceImageMemoryRequirements dr{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
        dr.pCreateInfo = &ici;
        VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vkGetDeviceImageMemoryRequirements(ctx.device(), &dr, &req);
        mMemBytes = (uint64_t)req.memoryRequirements.size * n;
    }

    mImageViews.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = mImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = mFormat.format;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &vci, nullptr, &mImageViews[i]));
    }

    // Per-IMAGE render-finished semaphores (one per swapchain image). Must be
    // (re)created here because the image count is only known now and can change
    // across recreate(); present() waits mRenderFinished[imageIndex].
    mRenderFinished.resize(n);
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx.device(), &rsci, nullptr, &mRenderFinished[i]));
    }
    return true;
}

void VkSwapchain::destroySwapchainObjects(VkDeviceContext& ctx) {
    for (VkSemaphore s : mRenderFinished) {
        if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    }
    mRenderFinished.clear();
    for (VkImageView v : mImageViews) {
        if (v) vkDestroyImageView(ctx.device(), v, nullptr);
    }
    mImageViews.clear();
    mImages.clear();
    mMemBytes = 0;
    if (mSwapchain) {
        vkDestroySwapchainKHR(ctx.device(), mSwapchain, nullptr);
        mSwapchain = VK_NULL_HANDLE;
    }
}

bool VkSwapchain::recreate(VkDeviceContext& ctx, uint32_t width, uint32_t height) {
    if (vkDeviceIsLost()) return false;   // #135
    vkDeviceWaitIdle(ctx.device());
    destroySwapchainObjects(ctx);
    return createSwapchainObjects(ctx, width, height);
}

void VkSwapchain::destroy(VkDeviceContext& ctx) {
    if (vkDeviceIsLost()) return;   // #135: leak to process exit, never touch the lost device
    if (ctx.device()) vkDeviceWaitIdle(ctx.device());
    // Per-frame-in-flight objects (image-available semaphores + fences).
    for (VkSemaphore s : mImageAvailable) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    for (VkFence f : mInFlight) if (f) vkDestroyFence(ctx.device(), f, nullptr);
    mImageAvailable.clear();
    mInFlight.clear();
    // Per-image render-finished semaphores are destroyed by destroySwapchainObjects.
    destroySwapchainObjects(ctx);
    // Surface is owned by the window layer; not destroyed here.
    mSurface = VK_NULL_HANDLE;
}

bool VkSwapchain::slotDone(VkDevice dev, uint32_t slot) const
{
    return dev && slot < mInFlight.size() && mInFlight[slot] && vkGetFenceStatus(dev, mInFlight[slot]) == VK_SUCCESS;
}

void VkSwapchain::waitFrameSlot(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return;   // (not created / destroyed)
    // The same hooked wait as acquire() (vk_common.h: device loss raises with this site,
    // an infinite wait is sliced into the hang watchdog). The fence is NOT reset here:
    // acquire() resets it only once a submit is certain.
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
}

bool VkSwapchain::acquire(VkDeviceContext& ctx, uint32_t* outImageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    if (vkDeviceIsLost()) return false;   // #135: no acquire on a lost device
    if (!mSwapchain) { if (outNeedsRecreate) *outNeedsRecreate = true; return false; }

    // Hooked (vk_common.h): DEVICE_LOST raises with this site (the #135 crash came
    // through here), an infinite wait is sliced into the hang watchdog. With frame pacing
    // (waitFrameSlot at the top of the main-loop frame) this fence is already signaled and
    // the wait returns at once; kept as the correctness backstop for slot reuse.
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    vkAddrFrameTick();

    VkResult r = vkAcquireNextImageKHR(ctx.device(), mSwapchain, mAcquireTimeout,
                                       mImageAvailable[mFrame], VK_NULL_HANDLE,
                                       outImageIndex);
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {
        // #1074 (VR only: the timeout is UINT64_MAX otherwise): no image in time, the semaphore
        // was not signaled, the fence is not reset -- skip the window's frame.
        return false;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // No image delivered and the semaphore was NOT signaled: skip the frame
        // and recreate. Do not reset the fence (no submit this frame).
        // #919: exclusive mode lost (alt-tab, another app took the display): the owner
        // re-acquires it or rebuilds without exclusive mode -- never a device loss. It does
        // not ask for a recreate: re-acquiring the mode is enough, and the owner falls back
        // to a rebuild without exclusive mode when that keeps failing.
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        // SUBOPTIMAL DID deliver an image and DID signal the semaphore; we must
        // render + present this frame (else the acquire semaphore stays signaled
        // with no matching wait). Flag recreate so the caller rebuilds AFTER
        // present. Fall through to the success path.
        if (outNeedsRecreate) *outNeedsRecreate = true;
    }
    else if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkAcquireNextImageKHR = %s\n",
                     vkResultString(r));
        // A lost device, or a lost surface (the window's surface is gone: nothing can be
        // presented again without re-creating the device's WSI objects): the graceful
        // device-lost exit (#135) instead of skipping every frame from now on.
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise("vkAcquireNextImageKHR", r, "graphics", __FILE__, __LINE__);
        return false;
    }
    // Only reset the fence once we know we'll submit work this frame.
    VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::beginFrameWithoutImage(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return false;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return false;
    VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::present(VkDeviceContext& ctx, uint32_t imageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    // Wait the render-finished semaphore for THIS image (signaled by its submit).
    pi.pWaitSemaphores = &mRenderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &mSwapchain;
    pi.pImageIndices = &imageIndex;

    if (vkDeviceIsLost()) return false;   // #135: nothing reaches a lost device
    VkResult r = VK_SUCCESS;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_PRESENT))
        r = VK_ERROR_DEVICE_LOST;          // RenderVKDebugSimulateDeviceLost 3: not presented
    else
        r = vkQueuePresentKHR(ctx.queue(), &pi);
    mFrame = (mFrame + 1) % mFramesInFlight;

    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) {
        // (#919: like OUT_OF_DATE, a MODE_LOST present still counts as enqueued -- its
        // semaphore wait executes -- so nothing is left signaled; no recreate, see acquire.)
        if (r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT) mExclusiveLost = true;
        else if (outNeedsRecreate) *outNeedsRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "[vkgfx] vkQueuePresentKHR = %s\n", vkResultString(r));
        if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR)
            vkDeviceLostRaise(vkDeviceLostSimulated() ? "vkQueuePresentKHR (simulated)" : "vkQueuePresentKHR",
                              r, "graphics", __FILE__, __LINE__);
        return false;
    }
    return true;
}
