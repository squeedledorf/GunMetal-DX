/**
 * @file llmemdiag.h
 * @brief Crash / disconnect / memory-failure evidence (#700): the memory status line,
 *        "last words" before a fatal error, the pre-disconnect block (recent frame times,
 *        region crossings, memory state), GPU-wait counts for long-frame lines and the
 *        allocation-failure counters.
 *
 * Everything here is privacy-safe: no avatar names, chat, region names or IPs.
 *
 * HOOK POINTS for the Vulkan allocation / RAM work (#697 / #698 / #699). All are cheap,
 * thread-safe and optional (nothing is logged for a hook nobody feeds):
 *   LLMemDiag::noteAllocFailure(kind, dropped)  -- an allocation failed; dropped=false: retried
 *                                                  later, true: the work was given up
 *   LLMemDiag::noteGpuWait(site, ms)            -- a vkDeviceWaitIdle / queue-idle / fence wait
 *                                                  at `site` (a string literal, e.g. __func__ or
 *                                                  "file:line") took ms on the main thread
 *   LLMemDiag::setSoftFailProvider(fn)          -- fn() = the VK soft-fail summary
 *                                                  (VkDeviceContext::softFailSummary)
 *   LLMemDiag::setExtraProvider(fn)             -- fn() = any further one-line state (RAM mesh /
 *                                                  decode backlog, grow refusals ...)
 * The main thread calls tick() / frameDone() (LLAppViewer::doFrame).
 */
#ifndef LL_LLMEMDIAG_H
#define LL_LLMEMDIAG_H

#include "stdtypes.h"
#include <functional>
#include <string>

namespace LLMemDiag
{
    enum AllocKind : U32
    {
        AK_VK_HOST = 0,     // vkAllocateMemory, host-visible
        AK_VK_DEVICE,       // vkAllocateMemory, device-local
        AK_VK_BUFFER,       // buffer / image creation
        AK_VK_PIPELINE,     // pipeline compile
        AK_RAM_TEXTURE,     // texture decode / raw image
        AK_RAM_MESH,        // mesh decode / LLVolume
        AK_RAM_OTHER,
        AK_COUNT
    };

    using Provider = std::function<std::string()>;

    // ---- hooks (any thread) ----
    void noteAllocFailure(AllocKind kind, bool dropped);
    void noteGpuWait(const char* site, double ms);
    void setSoftFailProvider(Provider fn);
    void setExtraProvider(Provider fn);

    // ---- main thread, once per frame ----
    // Logs the memory status line when the RAM governor state or the VRAM ladder rung /
    // mode changed (rate-limited), every 30 s while not calm, every 60 s while allocation
    // failures were counted.
    void tick();
    // The frame that just ended: feeds the 120-frame ring and snapshots / clears the
    // per-frame GPU-wait table (read it with lastFrameGpuWaits()).
    // vk_waits (#697/#699): the vkgfx per-frame wait summary, appended to the wait table.
    void frameDone(double frame_ms, U32 frame_number, const std::string& vk_waits = std::string());
    // " | gpu-waits: site xN T ms ..." for the frame that just ended; "" = none.
    std::string lastFrameGpuWaits();

    // ---- evidence ----
    // One line: "RAM <state> avail .. | VRAM ... | soft-fails ... | alloc-fail ..."
    std::string statusLine();
    // "vk-host 3r/1d vk-device 0 ..." (r = retried, d = dropped); "none" when zero.
    std::string allocFailText();
    void noteRegionCrossing(const char* what);   // main thread: "crossed" / "movement-complete"
    // Pre-disconnect block (frame times, crossings, memory state) -> the log. why: no names.
    void logPreDisconnect(const char* why);
    // Before a fatal error: memory state, last GPU passes, frame number. Once per process.
    void lastWords(const char* what);
}

#endif // LL_LLMEMDIAG_H
