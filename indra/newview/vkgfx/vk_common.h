// vktest — Vulkan de-risking spike for the Slipstream Vulkan renderer.
// vk_common.h — shared helpers, error checking, small utilities.
//
// This file is intentionally standalone: it depends only on the Vulkan headers,
// the C++ standard library, and glm. Nothing here touches the viewer.
#pragma once

#include <vulkan/vulkan.h>

// Vulkan 1.3 core -> 1.2 + extension entry-point routing (#675). Included BEFORE the hook
// macros below: its defaults name the real loader entry points.
#include "vk_compat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- error checking ---------------------------------------------------------

const char* vkResultString(VkResult r);
// The process-wide VkPipelineCache (owned by VkDeviceContext, persisted on disk by
// vk_pipecache.cpp). Pass it to EVERY vkCreate*Pipelines call. Internally synchronized
// (created without EXTERNALLY_SYNCHRONIZED): safe from the async compile threads.
// VK_NULL_HANDLE before device init / after shutdown.
VkPipelineCache vkSharedPipelineCache();
// On VK_ERROR_DEVICE_LOST: dump VK_EXT_device_fault info (fault addresses /
// vendor description, mapped against the GPU address registry) and the GPU
// breadcrumbs to stderr AND, line by line, to gVkFaultLogSink when set (the viewer
// routes it into its log, so user reports carry it). Says so when the driver offers
// no fault info.
void vkReportDeviceLost();
// Viewer hook: receives each line of the DEVICE_LOST report (no trailing newline).
extern void (*gVkFaultLogSink)(const char* line);
// GPU breadcrumbs (#136): mark a point in a command buffer with a static label.
// NVIDIA: VK_NV_device_diagnostic_checkpoints; AMD: VK_AMD_buffer_marker (last
// label started / finished). Enabled only when the driver lists the extension;
// vkReportDeviceLost prints where the GPU was. No-op otherwise. `name` must be a
// string with static storage (a literal).
void vkBreadcrumb(VkCommandBuffer cmd, const char* name);
// Set before device creation (RenderVKGpuBreadcrumbs); false keeps both extensions off.
extern bool gVkBreadcrumbsWanted;
// (#1128, RenderVKFaultVendorBinary; read before device creation) ask VK_EXT_device_fault for the driver's
// vendor binary crash dump (deviceFaultVendorBinary) and write it into gVkFaultDumpDir on a device loss.
extern bool gVkFaultVendorBinaryWanted;
extern std::string gVkFaultDumpDir;
// #973 DEBUG LABELS (RenderVKDebugLabels, set before instance creation): every vkBreadcrumb
// also records a VK_EXT_debug_utils command-buffer label (vkCmdInsertDebugUtilsLabelEXT) with
// the same static name, and objects named with setObjectName carry their debug names -- what
// Radeon GPU Detective, RenderDoc and other capture / crash tools show as the pass the GPU was
// in. The instance extension is requested only where the loader lists it. Point labels, not
// Begin / End regions: an End must balance a Begin on the same QUEUE, and our passes are split
// over the WSI pre-command buffer, the world buffer and the async compute segments.
extern bool gVkDebugLabelsWanted;
// Set before device creation (RenderVKDriverMemoryReport, #407): enable
// VK_EXT_device_memory_report when the driver offers it, so driver-internal allocations
// (device / queue / pipelines / descriptor pools / command memory ...) are attributed.
extern bool gVkMemoryReportWanted;
// Set before device creation (RenderVKTextureSuballoc*, #411): texture / UI images are
// suballocated from VMA pools -- size-class slabs up to the driver's allocation
// granularity (measured, #407), medium size-class slabs above it (#587: classes probed
// from the driver's memory requirements of SL texture shapes, or classesKB), and one
// TLSF fallback pool up to mediumMaxKB -- instead of one vkAllocateMemory each.
// enabled = false keeps every image dedicated (the old path).
struct VkTexPoolConfig
{
    bool     enabled = true;
    uint32_t smallBlockMB = 4;     // slab block size (raised to 16 slots for coarse classes)
    uint32_t mediumMaxKB = 2048;   // largest pooled image (memory requirement size)
    uint32_t mediumBlockMB = 8;    // medium (TLSF) block size (raised to 2x mediumMaxKB)
    // #587 medium size classes (RenderVKTexturePoolClassesKB): "" / "auto" = probed from
    // the driver, "default" = the built-in list, "none" = TLSF fallback only, or KiB
    // values (comma / space separated; may be combined with "auto" / "default").
    std::string classesKB;
};
extern VkTexPoolConfig gVkTexPoolConfig;

// Set before device creation (#695 UMA / integrated GPUs with a BIOS carve-out). An
// INTEGRATED_GPU whose largest non-device-local heap is larger than its device-local
// heap (AMD APUs: a 512 MB "VRAM" carve-out next to the shared system-memory heap) is
// UMA: both heaps are the same DRAM, so the VRAM budget is the carve-out PLUS a share
// of the host heap, and device-local allocations spill to a GPU-usable host-heap type
// once the carve-out is full (VkDeviceContext::pickDeviceLocalType). Intel / ANV (one
// big DEVICE_LOCAL heap) and every discrete card stay on the old path, unchanged.
struct VkUmaConfig
{
    bool     forceUma = false;        // RenderVKDebugForceUma: UMA mode on any device (testing)
    uint32_t fakeCarveoutMB = 0;      // RenderVKDebugFakeCarveoutMB: UMA only, clamp the carve-out budget
    uint32_t sharedRamPercent = 40;   // RenderVKUmaSharedRamPercent: host-heap share ceiling (% of RAM)
    uint64_t ramTotal = 0;            // physical RAM (LLMemoryGovernor::readSystemMemory), 0 = unknown
};
extern VkUmaConfig gVkUmaConfig;

// ---- ASYNC COMPUTE (roadmap phase 3a) ----
// Command buffers recorded for the async compute queue are registered here (by the
// renderer, when it allocates them). computeOnly = the queue's family has no graphics
// bit: every vkCmdPipelineBarrier2 recorded into such a buffer is SANITIZED -- graphics-
// only stages (VS / FS / fragment tests / attachment output / vertex + index input ...)
// and their accesses are dropped, an emptied side becomes COMPUTE_SHADER (so layout
// transitions still chain after the segment's semaphore wait and into its signal). The
// dropped graphics producers / consumers are covered by the cross-queue semaphores (the
// segment waits ALL_COMMANDS on the graphics work before its fork; graphics after the
// join waits on the segment). That is what lets the shared record* helpers (RT build, RT
// mask / SSAO, god rays) run unchanged on either queue. Breadcrumbs also use the registry
// (separate AMD marker words per queue).
void vkAsyncRegisterCmd(VkCommandBuffer cmd, bool computeOnly);
void vkAsyncUnregisterCmd(VkCommandBuffer cmd);
bool vkAsyncIsAsyncCmd(VkCommandBuffer cmd);
// #845 / #917: the async segment an async command buffer is recording now (its q_* label, set
// by the renderer at the fork; null = unlabelled). Diagnostics only.
void vkAsyncSetCmdLabel(VkCommandBuffer cmd, const char* label);
const char* vkAsyncCmdLabel(VkCommandBuffer cmd);
// #845 / #917: an image's registry name (setObjectName / vkImgName), "" when unknown or busy.
void vkImgNameOf(VkImage image, char* out, size_t n);
bool vkAsyncIsComputeOnlyCmd(VkCommandBuffer cmd);
uint32_t vkAsyncSanitizedBarriers();   // barriers changed so far (diagnostic counter)
// #845 / #917: attachment-layout (or depth / stencil) image transitions recorded into
// compute-only command buffers so far (each distinct kind is also logged once).
uint32_t vkAsyncAttachTransitions();
void vkqCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep);
// Every vkgfx call site goes through the sanitizer (a pointer compare when the command
// buffer is not a registered compute-only one). vk_device.cpp #undefs it to reach the
// real entry point.
#ifndef VK_NO_ASYNC_BARRIER_HOOK
#define vkCmdPipelineBarrier2 vkqCmdPipelineBarrier2
#endif
// vkDeviceWaitIdle's result used to be ignored everywhere: a GPU hang / fault was
// swallowed by the wait and blamed on the next checked call (often a submit, after
// the driver's multi-second timeout). Every vkgfx call goes through this instead: on
// VK_ERROR_DEVICE_LOST it raises (vkDeviceLostRaise) with the waiting call site; once
// lost it returns VK_ERROR_DEVICE_LOST without waiting; other results are unchanged.
VkResult vkqDeviceWaitIdle(VkDevice dev, const char* file, int line);
#ifndef VK_NO_ASYNC_BARRIER_HOOK
#define vkDeviceWaitIdle(dev) vkqDeviceWaitIdle((dev), __FILE__, __LINE__)
#endif
// ---- #699 GPU WAIT ACCOUNTING + REDUNDANT-IDLE ELISION ----
// Every vkDeviceWaitIdle / vkQueueWaitIdle (and every INFINITE vkWaitForFences /
// vkWaitSemaphores that actually blocked >= 0.5 ms) is counted per call site (file:line)
// with its total / worst time. A device idle with NO queue submission since the previous
// idle returned (any thread, any queue: every submit goes through vkqQueueSubmit[2]) is
// ELIDED -- the GPU has nothing that could still use the resource the caller is about to
// free -- so a frame that grows several buffers back to back (ensureRiggedBuffers: up to
// six waits) pays one wait instead of six. Elided idles are counted too (not timed).
// Thread-safe. For the #700 diagnostics:
struct VkWaitSiteStat
{
    const char* file = nullptr; int line = 0;
    const char* kind = "";           // "deviceIdle" / "queueIdle" / "fence" / "semaphore"
    uint64_t count = 0, elided = 0;  // waits that ran / device idles elided (nothing submitted since)
    double totalMs = 0.0, maxMs = 0.0;
};
// Every site seen so far (sorted by total time, largest first).
void vkWaitSiteStats(std::vector<VkWaitSiteStat>& out);
// Totals since the previous call (then reset): blocking waits, their ms, the worst single
// wait and its site. Call once per frame from the main loop for a per-frame figure.
struct VkWaitFrameStat { uint32_t waits = 0, elided = 0; double ms = 0.0, worstMs = 0.0; const char* worstFile = nullptr; int worstLine = 0; };
VkWaitFrameStat vkWaitFrameTake();
// Monotonic count of queue submissions (the elision's clock).
uint64_t vkSubmitSeq();

// ---- #697 LOW-MEMORY ACCOUNTING (soft failures that were absorbed) ----
// Every converted out-of-memory site notes what happened: RETRIED (kept what it had, the
// request comes back later) or DROPPED (the asset / request / frame was given up after its
// retries; it becomes eligible again later -- nothing is blacklisted for the session).
// Thread-safe (atomics). For the #700 diagnostics: vkLowMemStats / vkLowMemSummary.
enum VkLowMemKind : uint32_t
{
    VKLM_STAGING = 0,   // per-frame upload staging (stageFor / streamBuf / streamRuns)
    VKLM_TABLE,         // id-indexed scene tables / scratch growth (ensureCap, occlusion, particles, rig cache, alpha sort...)
    VKLM_TARGET,        // render targets re-created by a ladder rung / resize (scale, mirror, probes, temporal)
    VKLM_TEXTURE,       // a texture upload (image create) that failed
    VKLM_MESH,          // a geometry / mesh upload that failed
    VKLM_RT,            // ray-tracing acceleration structures / aux
    VKLM_PIPELINE,      // pipeline creation under memory pressure
    VKLM_FRAME,         // a whole frame dropped (vkEndCommandBuffer / vkQueueSubmit2 out of memory)
    VKLM_DESCRIPTOR,    // descriptor set allocation
    VKLM_VIEW,          // image view creation
    VKLM_COUNT
};
const char* vkLowMemKindName(uint32_t k);
void vkLowMemNote(VkLowMemKind k, bool dropped, uint64_t bytes = 0);
struct VkLowMemStats { uint64_t retried[VKLM_COUNT] = {}, dropped[VKLM_COUNT] = {}, bytes[VKLM_COUNT] = {}; };
VkLowMemStats vkLowMemStats();
// "staging r3/d0, target r1/d0" (kinds with any event only; "none" when nothing happened).
std::string vkLowMemSummary();
// An out-of-memory result (the soft paths' test for a non-allocation call).
inline bool vkIsOom(VkResult r) { return r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY; }
// Debug fault injection for the NON-allocation out-of-memory sites (SLVkInjectOomCalls N):
// every Nth call at an instrumented site (vkEndCommandBuffer / vkQueueSubmit2 of the world
// frame, vkCreateImageView, descriptor set allocation) is answered with
// VK_ERROR_OUT_OF_HOST_MEMORY without calling the driver. 0 = off.
void vkSetInjectOomCalls(uint32_t every);
bool vkInjectOomTake();
// ---- DEVICE LOST (#135) ----
// One process-wide state. Every point a lost device can surface -- queue submits (both
// queues), fence / timeline-semaphore waits, vkDeviceWaitIdle / vkQueueWaitIdle, fence
// status, query results, acquire / present, and VK_CHECK -- calls vkDeviceLostRaise
// instead of aborting. The FIRST raise sets the state (before anything else runs), then
// calls gVkDeviceLostHandler (the viewer: one delimited diagnostics block in the log +
// logs/slipstream_gpu_lost.txt; the message box and the clean exit follow on the main
// loop) or, without a handler, prints vkReportDeviceLost. Later raises are no-ops.
// Thread-safe. Once lost, the hooks below return VK_ERROR_DEVICE_LOST WITHOUT entering
// the driver (no submit, no wait: some drivers hang waiting on a dead device) and the
// record / submit / teardown entry points early-out, so cleanup never blocks on the GPU.
// `result` is what the call returned (VK_ERROR_DEVICE_LOST, VK_ERROR_SURFACE_LOST_KHR,
// or VK_TIMEOUT for the hang watchdog); `queue` names the queue ("graphics", "async
// compute") or is null.
bool vkDeviceIsLost();
void vkDeviceLostRaise(const char* call, VkResult result, const char* queue, const char* file, int line);
// True when the loss was produced by RenderVKDebugSimulateDeviceLost (the device is
// healthy: the fault / checkpoint queries, which require a lost device, are skipped).
bool vkDeviceLostSimulated();
// The name of a queue the device created ("graphics" / "async compute" / "other").
const char* vkQueueName(VkQueue q);
// RenderVKDebugSimulateDeviceLost (testing aid; never touches the driver): the next
// call at `site` behaves as if it returned VK_ERROR_DEVICE_LOST.
enum VkSimLostSite : int
{
    VK_SIM_LOST_NONE = 0,
    VK_SIM_LOST_SUBMIT = 1,    // next vkQueueSubmit / vkQueueSubmit2 (not submitted)
    VK_SIM_LOST_WAIT = 2,      // next vkWaitForFences / vkWaitSemaphores (as in #135's crash)
    VK_SIM_LOST_PRESENT = 3,   // next vkQueuePresentKHR (not presented)
};
void vkSimulateDeviceLost(int site);
// Consumes an armed simulation for `site`: true once (marks the loss as simulated).
bool vkSimulateDeviceLostTake(int site);
// The most recently RECORDED GPU marks (gpuSubMark / gpuTimerMark / FSR passes), oldest
// first, from a fixed ring kept by vkBreadcrumb whether or not a breadcrumb extension is
// available. Recorded, not executed: the NV checkpoints / AMD markers in the fault report
// say how far the GPU got. Returns the count written to out (<= max).
uint32_t vkRecentMarks(const char** out, uint32_t max);
// Viewer hook for the first raise (see above). Must return: the exit is driven from the
// main loop (vkDeviceIsLost()).
extern void (*gVkDeviceLostHandler)(const char* call, VkResult result, const char* queue,
                                    const char* file, int line);
// Viewer hook for any OTHER VK_CHECK failure (set by the app at Vulkan init): logs and
// leaves through the viewer's crash path (LL_ERRS: crash marker + dump). Not expected to
// return; VK_CHECK aborts if it does or when none is set (standalone tests). A bare
// abort() on Windows is __fastfail(7) / 0xC0000409: no dump and no log line (#107).
extern void (*gVkFatalHandler)(const char* expr, VkResult result, const char* file, int line);

// Device-lost aware wrappers (vk_common.h routes every vkgfx caller here, like
// vkDeviceWaitIdle above). Lost: return VK_ERROR_DEVICE_LOST without the driver call.
// Otherwise call through; VK_ERROR_DEVICE_LOST raises with the call site. Infinite
// fence / semaphore waits run in slices: a wait still blocked after 5 s logs a warning
// with the recent GPU marks, and after 60 s it is treated as a GPU hang (raised with
// VK_TIMEOUT) instead of blocking the main thread forever.
VkResult vkqQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f, const char* file, int line);
VkResult vkqQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f, const char* file, int line);
VkResult vkqWaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout,
                          const char* file, int line);
VkResult vkqWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* wi, uint64_t timeout, const char* file, int line);
VkResult vkqGetFenceStatus(VkDevice d, VkFence f, const char* file, int line);
VkResult vkqQueueWaitIdle(VkQueue q, const char* file, int line);
VkResult vkqGetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t* v, const char* file, int line);
VkResult vkqGetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                void* data, VkDeviceSize stride, VkQueryResultFlags flags,
                                const char* file, int line);
#ifndef VK_NO_ASYNC_BARRIER_HOOK
#define vkQueueSubmit(q, n, si, f) vkqQueueSubmit((q), (n), (si), (f), __FILE__, __LINE__)
#define vkQueueSubmit2(q, n, si, f) vkqQueueSubmit2((q), (n), (si), (f), __FILE__, __LINE__)
#define vkWaitForFences(d, n, f, all, t) vkqWaitForFences((d), (n), (f), (all), (t), __FILE__, __LINE__)
#define vkWaitSemaphores(d, wi, t) vkqWaitSemaphores((d), (wi), (t), __FILE__, __LINE__)
#define vkGetFenceStatus(d, f) vkqGetFenceStatus((d), (f), __FILE__, __LINE__)
#define vkQueueWaitIdle(q) vkqQueueWaitIdle((q), __FILE__, __LINE__)
#define vkGetSemaphoreCounterValue(d, s, v) vkqGetSemaphoreCounterValue((d), (s), (v), __FILE__, __LINE__)
#define vkGetQueryPoolResults(d, p, first, n, sz, data, stride, flags) \
    vkqGetQueryPoolResults((d), (p), (first), (n), (sz), (data), (stride), (flags), __FILE__, __LINE__)
#endif

// ---- GPU address registry (DEVICE_LOST forensics) ----
// Every buffer VkDeviceContext creates is recorded with its device-address range,
// usage and creating call site; destroyed ranges move to a ring of recent frees.
// Acceleration structures (sub-ranges of their storage buffers) are noted
// separately. vkReportDeviceLost maps each reported fault address against live
// and recently freed ranges. Thread-safe; always on (cheap: create/destroy only).
void vkAddrTrackBuffer(VkBuffer b, VkDeviceAddress a, VkDeviceSize size, uint32_t usage, const void* caller);
void vkAddrUntrackBuffer(VkBuffer b);
void vkAddrName(VkBuffer b, const char* name);            // label a tracked buffer
void vkAddrNote(VkDeviceAddress a, VkDeviceSize size, const char* kind, uint32_t id);
void vkAddrForget(VkDeviceAddress a);                      // a noted range was destroyed
void vkAddrFrameTick();                                    // once per frame (fault report ages)
uint32_t vkAddrCurrentFrame();                             // the registry's frame (vkAddrFrameTick count)
// #845 / #917: whole VkDeviceMemory blocks (texture-pool VMA blocks, alias-arena blocks): their VA
// range lives on in a long ring after the free, so a fault in a block whose last image record
// already left the image ring still names the block, its kind and the frame it was freed.
void vkMemBlockTrack(VkDeviceMemory mem, VkDeviceAddress va, VkDeviceSize size, const char* kind);
void vkMemBlockUntrack(VkDeviceMemory mem);
// #845 / #917: every queue submission (frame segments, submitImmediate, everything through
// vkqQueueSubmit[2]) goes into a small ring -- queue, call site, registry frame, host time --
// printed by the device-lost report, so out-of-frame submits are visible next to the crumbs.
void vkSubmitLogNote(VkQueue q, const char* what, const char* file, int line);

// ---- REGISTRY QUERIES (#874 RT scene audit) ----
// What a GPU address points into, by the registry. Classes: a live AS note, the start of
// one (a valid BLAS reference) or inside it; a recently freed AS note; a live buffer under
// no live AS note (for an AS storage chunk: arena space no live AS owns); a recently freed
// buffer; nothing known.
// #973: LIVE objects win. The driver reuses freed VA ranges, so a live buffer that is not AS
// storage (rt.scratch was reported as "FREED AS 'rig'") is the answer even when a freed AS
// note also covers the address; the freed note is kept as the secondary "previously freed
// range" (prev*). Inside live AS STORAGE (an arena chunk) a freed AS note still wins: there
// it is the dangling-BLAS evidence the RT scene audit needs (freed before / after the build).
enum VkAddrClass : uint8_t
{
    VKADDR_UNKNOWN = 0,
    VKADDR_AS_LIVE,          // a live AS note contains it (exact = its start)
    VKADDR_AS_FREED,         // a recently freed AS note contains it (no live one does)
    VKADDR_BUF_LIVE,         // a live buffer, under no live AS note
    VKADDR_BUF_FREED,        // a recently freed buffer only
};
struct VkAddrHit
{
    VkDeviceAddress lo = 0;
    VkDeviceSize size = 0;
    uint32_t id = 0;            // AS note id / buffer usage flags
    uint32_t frame = 0;         // created (registry frame)
    uint32_t freedFrame = 0;    // freed (registry frame; 0 = live)
    uint8_t cls = VKADDR_UNKNOWN;
    bool exact = false;         // the address is the range's start
    char name[24] = {};         // AS kind ("blas", "rig", ...) / buffer debug name
    // #973: a recently freed AS note / buffer that ALSO covers the address (the range the
    // live object reuses), for live classes only; prevKind 0 = none, 1 = AS, 2 = buffer.
    uint8_t prevKind = 0;
    uint32_t prevId = 0;        // its AS note id / buffer usage flags
    uint32_t prevFreedFrame = 0;
    char prevName[24] = {};
};
// Classify n addresses under ONE registry lock (try-lock: false = the registry is busy,
// out untouched). Freed matches prefer the newest free. Costs O(n log N + ring) -- crash /
// on-demand paths only, never per frame.
bool vkAddrClassify(const VkDeviceAddress* a, size_t n, VkAddrHit* out);
// The live AS notes nearest below (start <= a) / above (start > a) `a`, the recently freed
// ones likewise, and the highest live end address of any buffer or AS. False = busy.
struct VkAddrNeighbours
{
    VkAddrHit asBelow, asAbove, freedAsBelow, freedAsAbove;
    VkDeviceAddress liveTop = 0;
};
bool vkAddrNeighbourAS(VkDeviceAddress a, VkAddrNeighbours& out);
// #874 read guard: the device-address range of a tracked buffer (false = unknown / busy).
bool vkAddrOfBuffer(VkBuffer b, VkDeviceAddress& lo, VkDeviceSize& size);
// DEVICE_LOST report output (stderr + gVkFaultLogSink): one line, no trailing newline.
void vkFaultLogLine(const char* line);
// #874: called by vkReportDeviceLost after the fault report (also on simulated / unconfirmed
// losses, then with n = 0) with the reported data fault addresses. The RT foundation sets it
// (LLCleanRTVK: the RT scene audit). Mapped-memory reads only: no Vulkan call after a loss.
extern void (*gVkRtLostAuditHook)(const VkDeviceAddress* faults, uint32_t n);
// #845 / #917: called by vkReportDeviceLost before the RT audit: the async compute state (the
// last submitted frames' segments with their timeline waits / signals). The renderer sets it
// while its async queue exists. No Vulkan call after a loss.
extern void (*gVkAsyncLostHook)();
// The viewer's main loop calls this after a device loss (LLAppViewer::onGraphicsDeviceLost):
// when the loss was raised on a thread other than the one that records the RT build, the
// audit's CPU-table half runs here instead (null / no-op otherwise).
extern void (*gVkRtLostAuditMainLoop)();
// The fault addresses of the last DEVICE_LOST report (for an audit deferred to the main loop).
uint32_t vkLastFaultAddresses(VkDeviceAddress* out, uint32_t max);

// ---- IMAGE REGISTRY (DEVICE_LOST forensics, snapcrash #2) ----
// Every image VkDeviceContext creates is recorded: debug name (setObjectName), extent,
// layers, format, memory size, creating call site, created / freed frame and -- where
// the driver lets a buffer alias the image's (dedicated) memory -- the GPU virtual
// address range of that memory. vkReportDeviceLost maps fault addresses against live
// and recently freed images as well as buffers, and always lists the images freed in
// the last few frames: a READ_INVALID no buffer explains is usually a freed image that
// a stale descriptor still names. SLIPSTREAM_VK_NO_IMAGE_VA=1 skips the address probe
// (the image memory is then allocated exactly as before). Always on otherwise (cost:
// image create / destroy only).
void vkImgTrack(VkImage img, VkDeviceAddress va, VkDeviceSize size, uint32_t w, uint32_t h,
                uint32_t layers, VkFormat fmt, const void* caller);
void vkImgUntrack(VkImage img);
void vkImgName(VkImage img, const char* name);
// Every image VIEW gets a serial that is never reused (unlike the handle value).
void vkViewTrack(VkImageView v, VkImage img);
void vkViewUntrack(VkImageView v);

// ---- DESCRIPTOR TRACKING (RenderVKDebugDescriptorTracking; on with the snapshot stress) ----
// Enforces the render-target generation contract (llcleanrenderervk.h, mTargetGen) at
// bind time: every single image descriptor written (descriptorCount 1, array element 0;
// bindless arrays are not tracked) records its view and the view's serial; at
// vkCmdBindDescriptorSets a recorded serial that no longer matches -- the view was
// destroyed, or its handle now belongs to another image -- is logged once per (set,
// binding) with the image's name and its created / freed frame. A stale set becomes a
// log line on the CPU instead of a GPU page fault seconds later. Off: one relaxed
// atomic load per call. Turning it on forgets every earlier record (no false alarms
// from sets written while it was off).
void vkDescTrackEnable(bool on);
bool vkDescTrackEnabled();
uint32_t vkDescTrackStaleCount();   // stale binds seen so far (each logged once)
// ---- COPY-ON-WRITE SET TRACKING (LLCleanRendererVK: buffer re-size without a device wait) ----
// The renderer re-points some long-lived (not per-frame) descriptor sets to fresh copies
// instead of waiting for the device before rewriting them in place (a set a pending command
// buffer uses must not be updated). A copy takes only descriptors that were WRITTEN: writes /
// copies into a tracked set record which array elements (0..31) of which bindings (0..63)
// hold a descriptor, so an undefined one is never copied. A write past that range makes the
// set uncopyable (vkCowTracked false: the caller waits for the device instead) -- never a
// partial copy that leaves a recycled spare's stale descriptors in place (#926). Register a set right after it is
// allocated, before its first write. vkCowCopySet copies the written descriptors of a
// tracked `src` into `dst` (which becomes tracked with the same record; false: src is not
// tracked). The GUARD (calling thread only) counts every write / copy whose destination is
// not in the fresh list while it is on -- a set the re-point missed, updated while frames in
// flight may still use it; the first miss's set + binding are kept for the log. A newly
// allocated handle is untracked. Off: one relaxed load per update.
void     vkCowTrack(VkDescriptorSet set, bool on);          // start (empty record) / stop tracking
bool     vkCowTracked(VkDescriptorSet set);                 // tracked AND copyable
constexpr uint32_t kVkCowMaxArrayElems = 32;               // elements per binding a copy can carry
bool     vkCowCopySet(VkDevice dev, VkDescriptorSet src, VkDescriptorSet dst);
void     vkCowGuardBegin(const VkDescriptorSet* fresh, uint32_t n);   // `fresh` must outlive the window
void     vkCowGuardEnd();
uint32_t vkCowGuardMisses(uint64_t* first_set = nullptr, uint32_t* first_binding = nullptr);
void vkqUpdateDescriptorSets(VkDevice dev, uint32_t writeCount, const VkWriteDescriptorSet* writes,
                             uint32_t copyCount, const VkCopyDescriptorSet* copies);
void vkqCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout,
                              uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets,
                              uint32_t dynCount, const uint32_t* dyn);
VkResult vkqAllocateDescriptorSets(VkDevice dev, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* sets);
VkResult vkqCreateImageView(VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* cb,
                            VkImageView* out);
void vkqDestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks* cb);
#ifndef VK_NO_ASYNC_BARRIER_HOOK
#define vkUpdateDescriptorSets vkqUpdateDescriptorSets
#define vkCmdBindDescriptorSets vkqCmdBindDescriptorSets
#define vkAllocateDescriptorSets vkqAllocateDescriptorSets
#define vkCreateImageView vkqCreateImageView
#define vkDestroyImageView vkqDestroyImageView
#endif

// ---- NULL PIPELINE GUARD (#672) ----
// A pipeline whose creation ran out of memory is VK_NULL_HANDLE for the session (vk_pipeline.h:
// never fatal). Callers test their handles; as the last line of defence, binding a NULL
// pipeline is skipped (binding it is invalid) and the draws / dispatches recorded after it
// on that command buffer, until a valid pipeline of that bind point is bound, are skipped
// too (they would run with whatever was bound before, or with nothing). Tracked per
// recording thread; logged once per kind. Cost when no NULL pipeline was ever bound on the
// thread: one thread-local test per call.
void vkqCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipeline p);
void vkqCmdDraw(VkCommandBuffer cmd, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi);
void vkqCmdDrawIndexed(VkCommandBuffer cmd, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi);
void vkqCmdDrawIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride);
void vkqCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride);
void vkqCmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co,
                                    uint32_t maxN, uint32_t stride);
void vkqCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z);
void vkqCmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o);
uint32_t vkNullPipelineSkips();   // draws / dispatches skipped so far (diagnostic counter)
#ifndef VK_NO_ASYNC_BARRIER_HOOK
#define vkCmdBindPipeline vkqCmdBindPipeline
#define vkCmdDraw vkqCmdDraw
#define vkCmdDrawIndexed vkqCmdDrawIndexed
#define vkCmdDrawIndirect vkqCmdDrawIndirect
#define vkCmdDrawIndexedIndirect vkqCmdDrawIndexedIndirect
#define vkCmdDrawIndexedIndirectCount vkqCmdDrawIndexedIndirectCount
#define vkCmdDispatch vkqCmdDispatch
#define vkCmdDispatchIndirect vkqCmdDispatchIndirect
#endif

#define VK_CHECK(expr)                                                          \
    do {                                                                        \
        VkResult _vk_res = (expr);                                              \
        if (_vk_res != VK_SUCCESS) {                                            \
            std::fprintf(stderr, "[vktest] VK_CHECK failed: %s = %s (%d)\n"     \
                         "  at %s:%d\n", #expr, vkResultString(_vk_res),        \
                         (int)_vk_res, __FILE__, __LINE__);                     \
            if (_vk_res == VK_ERROR_DEVICE_LOST || vkDeviceIsLost()) {          \
                /* #135: the viewer exits from its main loop; the following  */ \
                /* GPU calls are no-ops on the lost device (vkDeviceIsLost). */ \
                vkDeviceLostRaise(#expr, _vk_res, nullptr, __FILE__, __LINE__); \
                if (gVkDeviceLostHandler) break;                                \
            } else if (gVkFatalHandler) {                                       \
                gVkFatalHandler(#expr, _vk_res, __FILE__, __LINE__);            \
            }                                                                   \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

// ---- logging ----------------------------------------------------------------

#define VK_LOG(...)                                                             \
    do {                                                                        \
        std::fprintf(stdout, "[vktest] " __VA_ARGS__);                          \
        std::fprintf(stdout, "\n");                                             \
        std::fflush(stdout);                                                    \
    } while (0)

// ---- push-constant budget (#238) ---------------------------------------------
// Every push block must fit the Vulkan spec minimum maxPushConstantsSize (128 B;
// AMD Polaris exposes exactly that). Larger per-view / per-pass data goes in a
// buffer indexed by a pushed slot (LLCleanRendererVK's param ring). Place before a
// vkCmdPushConstants with the pushed object / type.
#define VK_PUSH_BUDGET_BYTES 128u
#define VK_PUSH_BUDGET_CHECK(x) \
    static_assert(sizeof(x) <= VK_PUSH_BUDGET_BYTES, "push constant block over the 128 B Vulkan minimum (#238)")
