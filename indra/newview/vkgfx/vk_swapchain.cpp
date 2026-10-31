#include "vk_swapchain.h"

#include <algorithm>
#include <cstdio>

bool VkSwapchain::checkResult(VkResult result) const
{
    if (result == VK_SUCCESS) return true;
    // Keep the main-window error policy intact. Auxiliary allocation failures
    // return to their owner, which leaves the existing floater in the viewer.
    if (!mUIOnly) VK_CHECK(result);
    std::fprintf(stderr, "[vkgfx] auxiliary swapchain allocation: %s\n", vkResultString(result));
    if (result == VK_ERROR_DEVICE_LOST)
        vkDeviceLostRaise("auxiliary swapchain allocation", result, "graphics", __FILE__, __LINE__);
    return false;
}

bool VkSwapchain::init(VkDeviceContext& ctx, VkSurfaceKHR surface,
                       uint32_t width, uint32_t height, uint32_t framesInFlight) {
    mSurface = surface;
    mMaintenance = mMaintenance || ctx.swapchainMaintenance();
    mFrame = 0;
    mUIFailed = false;
    mPresentPending = false;
    mUnresolvedPresent = false;
    mFramesInFlight = framesInFlight ? framesInFlight : 2;

    // Sanity: confirm the graphics queue family can present to this surface.
    VkBool32 presentSupported = VK_FALSE;
    if (!checkResult(vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.queueFamily(),
                                         mSurface, &presentSupported))) return false;
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
    if (mUIOnly) mSubmissionArmed.resize(mFramesInFlight, false);
    mImageAvailable.resize(mFramesInFlight);
    mInFlight.resize(mFramesInFlight);
    if (mUIOnly)
    {
        mAcquireArmed.resize(mFramesInFlight, false);
        mAcquireFences.resize(mFramesInFlight);
    }
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first acquire doesn't hang
    for (uint32_t i = 0; i < mFramesInFlight; ++i) {
        if (!checkResult(vkCreateSemaphore(ctx.device(), &sci, nullptr, &mImageAvailable[i])) ||
            !checkResult(vkCreateFence(ctx.device(), &fci, nullptr, &mInFlight[i])))
            return false;
        if (mUIOnly && !checkResult(vkCreateFence(ctx.device(), &fci, nullptr, &mAcquireFences[i]))) return false;
    }
    return true;
}

bool VkSwapchain::sVSync = true;
uint32_t VkSwapchain::sNoVsyncMode = 0;

bool VkSwapchain::createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                         uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (!checkResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        ctx.physicalDevice(), mSurface, &caps))) return false;

    // Choose a format: prefer BGRA8 SRGB, else the first available.
    uint32_t fmtCount = 0;
    if (!checkResult(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                         &fmtCount, nullptr))) return false;
    if (fmtCount == 0) {
        std::fprintf(stderr, "[vkgfx] surface has no formats\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    if (!checkResult(vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), mSurface,
                                         &fmtCount, fmts.data()))) return false;
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
    if (mUIOnly || !sVSync)
    {
        std::vector<VkPresentModeKHR> pms;
        // #919: with a full-screen-exclusive chain, the modes valid for it.
        if (!(mCreateChain && mPresentModeQuery && mPresentModeQuery(pms)))
        {
            pms.clear();
            uint32_t pmCount = 0;
            if (!checkResult(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, nullptr))) return false;
            pms.resize(pmCount);
            if (pmCount)
                if (!checkResult(vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.physicalDevice(), mSurface, &pmCount, pms.data()))) return false;
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
    if (mUIOnly && (mExtent.width > 8192 || mExtent.height > 8192 || imageCount > 4 ||
        uint64_t(mExtent.width) * mExtent.height * 4 * imageCount > 128ull * 1024 * 1024)) return false;
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
    if (mUIOnly) ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single queue family
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (mUIOnly)
    {
        for (auto mode : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                          VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & mode) { ci.compositeAlpha = mode; break; }
    }
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
    if (!checkResult(cr)) return false;

    uint32_t n = 0;
    if (!checkResult(vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, nullptr))) return false;
    if (mUIOnly && (!n || n > 4)) return false;
    mImages.resize(n);
    if (!checkResult(vkGetSwapchainImagesKHR(ctx.device(), mSwapchain, &n, mImages.data()))) return false;

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
    mRenderFinished.resize(n);
    if (!mUIOnly) mMaintenance = ctx.swapchainMaintenance();
    if (mMaintenance)
    {
        try
        {
            mPresentArmed.resize(n + mFramesInFlight, false);
            mPresentFences.resize(n + mFramesInFlight);
        }
        catch (const std::bad_alloc&)
        {
            if (mUIOnly) return false;
            mPresentArmed.clear();
            mPresentFences.clear();
            mMaintenance = false; // optional main proof allocation never fails the viewer
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = mImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = mFormat.format;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!checkResult(vkCreateImageView(ctx.device(), &vci, nullptr, &mImageViews[i])))
            return false;
    }

    // Per-IMAGE render-finished semaphores (one per swapchain image). Must be
    // (re)created here because the image count is only known now and can change
    // across recreate(); present() waits mRenderFinished[imageIndex].
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        if (!checkResult(vkCreateSemaphore(ctx.device(), &rsci, nullptr, &mRenderFinished[i])))
            return false;
    }
    if (mMaintenance)
    {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (auto& fence : mPresentFences)
        {
            const VkResult result = vkCreateFence(ctx.device(), &fci, nullptr, &fence);
            if (result == VK_SUCCESS) continue;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                vkDeviceLostRaise("present proof allocation", result, "graphics", __FILE__, __LINE__);
                return false;
            }
            if (mUIOnly) return false;
            // Main already has a usable chain: keep it with the conventional
            // existing idle-teardown policy, then retry proofs at next recreate.
            for (VkFence allocated : mPresentFences) if (allocated) vkDestroyFence(ctx.device(), allocated, nullptr);
            mPresentFences.clear();
            mPresentArmed.clear();
            mMaintenance = false;
            std::fprintf(stderr, "[vkgfx] optional main present-proof allocation refused: %s\n", vkResultString(result));
            break;
        }
    }
    return true;
}

void VkSwapchain::destroySwapchainObjects(VkDeviceContext& ctx) {
    for (VkFence fence : mPresentFences) if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
    mPresentFences.clear();
    mPresentArmed.clear();
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
    if (mUIOnly)
    {
        if (!retired(ctx.device())) return false;
    }
    else
    {
        vkDeviceWaitIdle(ctx.device());
        // Device-wide maintenance1: main WSI teardown also needs present proof.
        for (size_t i = 0; i < mPresentFences.size(); ++i)
            if (mPresentArmed[i]) VK_CHECK(vkWaitForFences(ctx.device(), 1, &mPresentFences[i], VK_TRUE, UINT64_MAX));
    }
    destroySwapchainObjects(ctx);
    return createSwapchainObjects(ctx, width, height);
}

void VkSwapchain::destroy(VkDeviceContext& ctx) {
    if (vkDeviceIsLost()) return;   // #135: leak to process exit, never touch the lost device
    if (mUIOnly)
    {
        if (!retired(ctx.device())) return; // owner retains this entry and polls
    }
    else if (ctx.device())
    {
        vkDeviceWaitIdle(ctx.device());
        for (size_t i = 0; i < mPresentFences.size(); ++i)
            if (mPresentArmed[i]) VK_CHECK(vkWaitForFences(ctx.device(), 1, &mPresentFences[i], VK_TRUE, UINT64_MAX));
    }
    for (VkFence fence : mAcquireFences) if (fence) vkDestroyFence(ctx.device(), fence, nullptr);
    mAcquireFences.clear();
    mAcquireArmed.clear();
    mSubmissionArmed.clear();
    // Per-frame-in-flight objects (image-available semaphores + fences).
    for (VkSemaphore s : mImageAvailable) if (s) vkDestroySemaphore(ctx.device(), s, nullptr);
    for (VkFence f : mInFlight) if (f) vkDestroyFence(ctx.device(), f, nullptr);
    mImageAvailable.clear();
    mInFlight.clear();
    // Per-image render-finished semaphores are destroyed by destroySwapchainObjects.
    destroySwapchainObjects(ctx);
    // Surface is owned by the window layer; not destroyed here.
    mSurface = VK_NULL_HANDLE;
    mPresentPending = false;
}

bool VkSwapchain::retired(VkDevice device) const
{
    if (vkDeviceIsLost() || mUnresolvedPresent || needsPresentDrain()) return false;
    for (size_t i = 0; i < mInFlight.size(); ++i)
        if ((!mUIOnly || mSubmissionArmed[i]) && vkGetFenceStatus(device, mInFlight[i]) != VK_SUCCESS) return false;
    for (size_t i = 0; i < mAcquireFences.size(); ++i)
        if (mAcquireArmed[i] && vkGetFenceStatus(device, mAcquireFences[i]) != VK_SUCCESS) return false;
    for (size_t i = 0; i < mPresentFences.size(); ++i)
        if (mPresentArmed[i] && vkGetFenceStatus(device, mPresentFences[i]) != VK_SUCCESS) return false;
    return true;
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
    if (mUIOnly)
    {
        if (!slotDone(ctx.device(), mFrame)) return false;
        if (mAcquireArmed[mFrame] && vkGetFenceStatus(ctx.device(), mAcquireFences[mFrame]) != VK_SUCCESS) return false;
        mAcquireArmed[mFrame] = false;
        if (!checkResult(vkResetFences(ctx.device(), 1, &mAcquireFences[mFrame]))) return false;
    }
    else VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    if (mMaintenance)
    {
        mNextPresentFence = 0;
        while (mNextPresentFence < mPresentFences.size() && mPresentArmed[mNextPresentFence] &&
               vkGetFenceStatus(ctx.device(), mPresentFences[mNextPresentFence]) != VK_SUCCESS) ++mNextPresentFence;
        if (mNextPresentFence == mPresentFences.size()) return false;
    }
    vkAddrFrameTick();

    VkResult r = vkAcquireNextImageKHR(ctx.device(), mSwapchain, mAcquireTimeout,
                                       mImageAvailable[mFrame], mUIOnly ? mAcquireFences[mFrame] : VK_NULL_HANDLE,
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
    if (mUIOnly && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)) mAcquireArmed[mFrame] = true;
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
        if (mUIOnly) mUIFailed = true;
        if (r == VK_ERROR_DEVICE_LOST || (r == VK_ERROR_SURFACE_LOST_KHR && !mUIOnly))
            vkDeviceLostRaise("vkAcquireNextImageKHR", r, "graphics", __FILE__, __LINE__);
        return false;
    }
    // Only reset the fence once we know we'll submit work this frame.
    if (mUIOnly) mSubmissionArmed[mFrame] = false;
    if (mUIOnly)
    {
        if (!checkResult(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]))) { mUIFailed = true; return false; }
    }
    else VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::beginFrameWithoutImage(VkDeviceContext& ctx) {
    if (vkDeviceIsLost() || !ctx.device()) return false;   // #135
    if (mFrame >= mInFlight.size() || !mInFlight[mFrame]) return false;
    if (mUIOnly)
    {
        if (!slotDone(ctx.device(), mFrame)) return false;
        if (mAcquireArmed[mFrame] && vkGetFenceStatus(ctx.device(), mAcquireFences[mFrame]) != VK_SUCCESS) return false;
        mAcquireArmed[mFrame] = false;
        if (!checkResult(vkResetFences(ctx.device(), 1, &mAcquireFences[mFrame]))) return false;
    }
    else VK_CHECK(vkWaitForFences(ctx.device(), 1, &mInFlight[mFrame], VK_TRUE, UINT64_MAX));
    if (vkDeviceIsLost()) return false;
    if (mUIOnly) mSubmissionArmed[mFrame] = false;
    if (mUIOnly)
    {
        if (!checkResult(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]))) { mUIFailed = true; return false; }
    }
    else VK_CHECK(vkResetFences(ctx.device(), 1, &mInFlight[mFrame]));
    return true;
}

bool VkSwapchain::present(VkDeviceContext& ctx, uint32_t imageIndex,
                          bool* outNeedsRecreate) {
    if (outNeedsRecreate) *outNeedsRecreate = false;
    VkSwapchainPresentFenceInfoEXT presentFence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (mMaintenance)
    {
        // Reserved before acquire. An acquire may return before the prior PE
        // fence signals, so proof slots must be independent of image indices.
        if (mNextPresentFence >= mPresentFences.size()) { mUIFailed = true; return false; }
        mPresentArmed[mNextPresentFence] = false;
        const VkResult reset = vkResetFences(ctx.device(), 1, &mPresentFences[mNextPresentFence]);
        if (reset != VK_SUCCESS)
        {
            mUIFailed = true;
            if (outNeedsRecreate) *outNeedsRecreate = true;
            if (reset == VK_ERROR_DEVICE_LOST)
                vkDeviceLostRaise("present proof reset", reset, "graphics", __FILE__, __LINE__);
            return false;
        }
        presentFence.swapchainCount = 1;
        presentFence.pFences = &mPresentFences[mNextPresentFence];
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.pNext = mMaintenance ? &presentFence : nullptr;
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
    if (r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) mPresentPending = true;
    if (mMaintenance && (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR ||
        r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR ||
        r == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)) mPresentArmed[mNextPresentFence] = true;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR &&
        r != VK_ERROR_SURFACE_LOST_KHR && r != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT &&
        r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_DEVICE_LOST)
        mUnresolvedPresent = true; // unknown enqueue outcome is never a completion proof
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
        if (mUIOnly) mUIFailed = true;
        if (r == VK_ERROR_DEVICE_LOST || (r == VK_ERROR_SURFACE_LOST_KHR && !mUIOnly))
            vkDeviceLostRaise(vkDeviceLostSimulated() ? "vkQueuePresentKHR (simulated)" : "vkQueuePresentKHR",
                              r, "graphics", __FILE__, __LINE__);
        return false;
    }
    return true;
}
