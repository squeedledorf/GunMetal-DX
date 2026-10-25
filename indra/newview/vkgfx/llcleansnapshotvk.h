/**
 * @file llcleansnapshotvk.h
 * @brief LIVE SNAPSHOTS (#873): snapshots taken from the main render path instead of an
 *        offscreen re-render.
 *
 * WHY. The offscreen snapshot renderer (LLVulkanContext::renderOffscreen -> recordWorld with
 * mOffscreen) kept diverging from the live view: RT reflections refuse offscreen, split
 * lighting is on-screen only, the weather / snow differed, and a big shot re-created a whole
 * render-target set at the shot size behind vkDeviceWaitIdle. A snapshot taken from the live
 * frame is what the user sees, for every current and future effect, and needs only the
 * output image (plus an accumulation buffer for upscaled shots) -- never a second set of
 * render targets.
 *
 * WHAT. One shot at a time, armed on the main thread (LLVulkanContext::beginLiveSnapshot),
 * recorded by the renderer at its capture point of the next live frame(s), read back after
 * that frame's fence (no vkDeviceWaitIdle):
 *  - MODE_RESAMPLE (shots at or below the window size, or upscaled without antialiasing):
 *    the swap image at the capture point (1 world only -- post-tonemap, before the selection
 *    outline, HUD and UI; 2 world + HUD; 3 the whole frame with UI) is copied into a buffer
 *    and resampled by a separable Lanczos-3 in linear light (sRGB decoded / re-encoded) to
 *    the shot size, cropped to the shot's aspect. One live frame: no hitch, no history reset.
 *  - MODE_DEPTH: the live depth buffer (render size, point 1), linearised with the frame's
 *    own inverse projection, point-sampled to the shot size, [near, far] -> 8-bit grey.
 *  - MODE_ACCUM (shots above the window size): `frames` live frames, each rendered with a
 *    distinct sub-pixel projection jitter (Halton(2,3) index jitterIndex(); the renderer
 *    skips the TAA / FSR resolve for them so the raw jittered samples reach the swap image),
 *    are splatted into a shot-size running mean (half-float RGB + weight, 8 B / px) with a
 *    Gaussian in output pixels -- super-resolution of a still scene. Content that moves
 *    (motion vectors, when the scene is not frozen) is not accumulated; the resolve takes the
 *    last frame there. The live view keeps presenting while it runs.
 * Every pass is a compute dispatch (clean_snap_live.comp); only the final packed RGB image
 * (LLImageRaw layout: 3 B / px, rows bottom-up) is host-visible.
 *
 * MEMORY. Device-local: the source copy (window size x 4 B; depth: render size x 4 B), the
 * horizontal pass's rows (shot width x the crop's rows x 8 B) or the accumulator (shot size
 * x 8 B), the motion copy (render size x 4 B, unfrozen accumulation only). Host-visible: the
 * output (shot size x 3 B). All allocated soft (VKMEM_TARGET / VKMEM_READBACK) at arm time --
 * a failure returns false with the reason so the caller falls back (a smaller shot, then the
 * offscreen path) -- and freed as soon as the shot is read (snapshot-only VRAM).
 * ML UPSCALER HOOK: shots above RenderSnapshotMaxUpscale x the window are accumulated at that
 * factor and scaled the rest of the way on the CPU (LLViewerWindow::rawSnapshot); a learned
 * single-image upscaler (clearly labelled, never the default) would replace that step.
 */
#ifndef LL_LLCLEANSNAPSHOTVK_H
#define LL_LLCLEANSNAPSHOTVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <cstdint>
#include <string>
#include <vector>

class LLCleanSnapshotVK
{
public:
    enum Mode : uint32_t { MODE_RESAMPLE = 0, MODE_DEPTH = 1, MODE_ACCUM = 2 };

    struct Request
    {
        uint32_t point = 1;              // capture point: 1 world, 2 world + HUD, 3 full frame with UI
        Mode     mode = MODE_RESAMPLE;
        uint32_t outW = 0, outH = 0;     // the shot (output) size
        float    crop[4] = {0.f, 0.f, 1.f, 1.f};   // normalised source rectangle x0, y0, w, h
        uint32_t frames = 1;             // MODE_ACCUM: jittered live frames (<= 64)
        bool     motionReject = false;   // MODE_ACCUM: drop moving pixels by motion vectors
        bool     dither = true;          // MODE_ACCUM: TPDF-dither the resolved 8-bit output once (#932)
        float    depthRange[2] = {0.1f, 128.f};    // MODE_DEPTH: metres mapped to 0 .. 255
    };

    // What the renderer hands over at a capture point.
    struct Source
    {
        uint32_t point = 0;
        // The swap image, COLOR_ATTACHMENT_OPTIMAL (restored after the copy).
        VkImage  swapImage = VK_NULL_HANDLE;
        VkFormat swapFormat = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        // The world depth, DEPTH_ATTACHMENT_OPTIMAL (point 1 only; restored after the copy).
        VkImage  depthImage = VK_NULL_HANDLE;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        uint32_t depthW = 0, depthH = 0;
        // Motion vectors (RG16F, SHADER_READ_ONLY_OPTIMAL; restored); null = none.
        VkImage  motionImage = VK_NULL_HANDLE;
        uint32_t motionW = 0, motionH = 0;
        float    jitterPx[2] = {0.f, 0.f};   // this frame's projection jitter, display px (x right, y down)
        float    invProj[4] = {0.f, 0.f, 0.f, 1.f};   // inverse projection {[10], [14], [11], [15]}
    };

    // Formats the RGB pass can unpack (8-bit RGBA / BGRA, UNORM or SRGB storage).
    static bool sourceFormatSupported(VkFormat f);

    // Main thread. Allocates the shot's buffers (soft); srcW x srcH: the swap image (colour
    // modes; the depth source is sized at its first record). False (with `why`) when the
    // pipelines or the memory are not available: nothing is armed.
    bool arm(VkDeviceContext* ctx, const Request& r, uint32_t srcW, uint32_t srcH, std::string& why);
    bool armed() const { return mState == ST_ARMED; }
    // MODE_ACCUM frames still to render: the renderer jitters this frame with
    // Halton(jitterIndex()) over jitterPhases() and skips the TAA / FSR resolve.
    bool jitterActive() const { return mState == ST_ARMED && mReq.mode == MODE_ACCUM; }
    uint32_t jitterIndex() const { return mFrame + 1u; }
    uint32_t jitterPhases() const { return mReq.frames; }
    uint32_t point() const { return mReq.point; }
    // The renderer, at each capture point of a live (on-screen) frame.
    void record(VkCommandBuffer cmd, const Source& s);
    // The context, after the frame's submission (its in-flight fence).
    void frameSubmitted(VkFence fence);
    // The renderer: the frame was dropped (#697) -- its recording never ran.
    void frameDropped();
    // Every frame recorded and submitted (read() can wait), or failed.
    bool finished() const { return mState == ST_SUBMITTED || mState == ST_FAILED; }
    bool failed() const { return mState == ST_FAILED; }
    const std::string& failReason() const { return mWhy; }
    // Waits for the last frame's fence, returns the image (LLImageRaw RGB, rows bottom-up)
    // and frees the shot's buffers. False on failure (freed as well).
    bool read(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h);
    // Abandon an armed / unread shot (waits for the device when frames are in flight).
    void cancel();
    void destroy();
    // This shot's allocations (bytes), for the log / the VRAM comparison.
    uint64_t deviceBytes() const;
    uint64_t hostBytes() const { return mOut.size; }

private:
    enum State : uint32_t { ST_IDLE = 0, ST_ARMED, ST_RECORDED, ST_SUBMITTED, ST_FAILED };
    bool ensurePipelines(VkDevice dev);
    bool ensureSource(VkDeviceSize bytes);
    bool ensureMotion(VkDeviceSize bytes);
    void writeSet();
    void freeBuffers();
    void fail(const std::string& why);

    VkDeviceContext* mCtx = nullptr;
    State     mState = ST_IDLE;
    Request   mReq;
    std::string mWhy;
    uint32_t  mSrcW = 0, mSrcH = 0;     // the source the shot was armed for (colour modes)
    uint32_t  mFrame = 0;               // frames recorded so far
    bool      mMotionNow = false;       // the accumulation reads motion vectors (first frame decides)
    bool      mSetDirty = true;
    bool      mPendingInFlight = false; // recorded frames not yet known complete
    VkFence   mFence = VK_NULL_HANDLE;  // the last recorded frame's in-flight fence
    uint32_t  mWorkRow0 = 0, mWorkRows = 0;   // MODE_RESAMPLE: source rows the H pass covers
    double    mArmTime = 0.0;

    GpuBuffer mSrc{}, mWork{}, mOut{}, mMotion{};

    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mSet = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;
    VkPipeline            mPipe[5] = {};   // H, V, DEPTH, ACC, RESOLVE
    bool                  mPipeFailed = false;
};

#endif // LL_LLCLEANSNAPSHOTVK_H
