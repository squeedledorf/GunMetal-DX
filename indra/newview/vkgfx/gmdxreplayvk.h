/**
 * @file gmdxreplayvk.h
 * @brief GunMetal-DX instant replay: the last seconds of the frame, encoded on the GPU.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// <GMDX> INSTANT REPLAY. Every captured frame (the finished swapchain image, UI included) is
// copied, scaled and converted to 4:2:0 on the graphics queue, then H.264-encoded on the GPU's
// video encode queue (VK_KHR_video_encode_h264). The encoded frames stay in memory for the last
// N seconds; saving writes them to an MP4 on a worker thread. Nothing is read back but the
// compressed bitstream.
//
// Plain Vulkan + std only (no viewer types), so the standalone test program can drive it; the
// viewer side (settings, menu, notifications) is newview/gmdxreplay.cpp.
//
// Stream: H.264 Main, I + P frames with one reference, an IDR every second (so a clip can start
// within a second of the asked length), rate controlled (VBR where the driver has it).

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// An Opus track for saveAsync (newview/gmdxreplayaudio.cpp makes it): 20 ms packets back to back,
// the first one starting at startSec on the same clock as nowSec. The writer trims it to the video.
struct GMDXReplayAudioClip
{
    double startSec = 0.0;
    uint32_t channels = 2;
    uint32_t preSkip = 0;                        // encoder lookahead, 48 kHz samples
    std::vector<std::vector<uint8_t>> packets;   // 960 samples each
};

class GMDXReplayEncoder
{
public:
    struct Device
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t gfxFamily = ~0u;
        VkQueue gfxQueue = VK_NULL_HANDLE;
        uint32_t encodeFamily = ~0u;
        VkQueue encodeQueue = VK_NULL_HANDLE;
    };

    struct Config
    {
        uint32_t maxHeight = 1080;       // the picture is scaled down to this height (aspect kept)
        uint32_t fps = 60;               // capture rate cap (frames between are skipped)
        uint32_t bitrateKbps = 20000;
        uint32_t seconds = 30;           // kept in memory
    };

    // DEVICE CREATION: the extensions the encoder needs, and the queue family to create a queue
    // of. False when the device can't encode H.264 (then nothing is enabled).
    static bool deviceSupport(VkPhysicalDevice physical, uint32_t& encodeFamily);
    static const std::vector<const char*>& deviceExtensions();

    GMDXReplayEncoder();
    ~GMDXReplayEncoder();

    // spv: the gmdx_replay_nv12.comp module. False + err when anything is missing.
    bool init(const Device& dev, const uint32_t* spv, size_t spvBytes, std::string& err);
    void shutdown();   // waits for the queues it used; call before the device goes
    void abandon();    // the device is lost: join the save thread, leak the Vulkan objects (no calls)
    bool ready() const { return mReady; }

    // PER FRAME, on the graphics command buffer that finished the frame: src is the finished
    // image (COLOR_ATTACHMENT_OPTIMAL before and after). Captures at most cfg.fps times a
    // second, when an encode slot is free; nowSec = a monotonic clock. True when it recorded.
    bool recordCapture(VkCommandBuffer cmd, VkImage src, VkFormat srcFormat, uint32_t w, uint32_t h,
                       double nowSec, const Config& cfg);
    // After the frame's last graphics submission: queues the encode of what recordCapture took.
    void submit();
    // The frame was dropped instead of submitted (its capture never ran): forget the capture.
    void cancel();
    // Harvests finished encodes into the replay buffer (cheap; call every frame).
    void poll(double nowSec);

    // SAVE the last `seconds` (from the first IDR inside them) to an MP4, on a worker thread, with
    // the audio track when audio has packets. False when there's nothing to save or a save is still running.
    bool saveAsync(const std::string& path, uint32_t seconds, double nowSec, GMDXReplayAudioClip audio = {});
    // A finished save's outcome (once per save): true when there was one to report.
    bool takeSaveResult(bool& ok, std::string& message);
    bool saving() const;
    // A problem worth telling the user about (once): true when there was one.
    bool takeError(std::string& message);

    // Seconds held in memory now / what the encoder is doing (log lines).
    double bufferedSeconds() const;
    std::string describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    bool mReady = false;
};
