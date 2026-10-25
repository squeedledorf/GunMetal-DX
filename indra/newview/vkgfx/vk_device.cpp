#include "vk_device.h"
#include <sstream>
#include "vk_pipecache.h"
#include "vk_pipeline.h"   // vkSetPipelineStatsCapture (RenderVKPipelineStats)
#include "vk_vma.h"
#include "slenv.h"
#include "gmdxreplayvk.h"   // <GMDX> instant replay: the encode queue + video extensions

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <dlfcn.h>
#endif

// ---------------------------------------------------------------------------
// VkResult -> string (small subset that we actually might hit)
// ---------------------------------------------------------------------------
// ---- VK_EXT_device_fault (DEVICE_LOST diagnostics) ----
static bool                       sFaultExtListed = false;
static bool                       sFaultEnabled   = false;
static VkDevice                   sFaultDevice    = VK_NULL_HANDLE;
static PFN_vkGetDeviceFaultInfoEXT sGetFaultInfo  = nullptr;

// ---- GPU breadcrumbs (DEVICE_LOST: where was the GPU?) ----
static bool                        sCrumbNVListed  = false;
static bool                        sCrumbAMDListed = false;
static VkQueue                     sCrumbQueue     = VK_NULL_HANDLE;
static VkQueue                     sCrumbQueue2    = VK_NULL_HANDLE;   // async compute queue (phase 3a)
static PFN_vkCmdSetCheckpointNV    sCmdSetCheckpointNV = nullptr;
static PFN_vkGetQueueCheckpointDataNV sGetQueueCheckpointDataNV = nullptr;
static PFN_vkCmdWriteBufferMarkerAMD sCmdWriteBufferMarkerAMD = nullptr;
static GpuBuffer                   sCrumbBuf{};    // AMD: [0] last started seq, [1] last finished seq
static constexpr uint32_t          CRUMB_RING = 1024;
static const char*                 sCrumbNames[CRUMB_RING] = {};
static uint32_t                    sCrumbSeq = 0;
bool gVkBreadcrumbsWanted = true;
// #973 debug labels (vk_common.h): the instance-level entry point, null = off / not listed.
bool gVkDebugLabelsWanted = true;
static PFN_vkCmdInsertDebugUtilsLabelEXT sCmdInsertLabel = nullptr;
static bool                        sDebugUtilsOn   = false;   // the instance has VK_EXT_debug_utils
bool gVkMemoryReportWanted = true;
VkTexPoolConfig gVkTexPoolConfig;
VkUmaConfig gVkUmaConfig;   // #695
// CPU ring of the most recently RECORDED marks (vkRecentMarks): fixed size, always on.
static constexpr uint32_t          MARK_RING = 64;
static std::atomic<const char*>    sMarkRing[MARK_RING] = {};
static std::atomic<uint32_t>       sMarkSeq{0};

// ---- DEVICE LOST state (#135; vk_common.h) ----
static std::atomic<bool>           sDeviceLost{false};
static std::atomic<bool>           sLostRaised{false};
static std::atomic<bool>           sLostSimulated{false};
// The DRIVER reported VK_ERROR_DEVICE_LOST (not a simulation, the hang watchdog or a lost
// surface): only then may the fault / checkpoint queries run (they require a lost device;
// on a wedged-but-not-lost GPU they could block too).
static std::atomic<bool>           sLostConfirmed{false};
static std::atomic<int>            sSimSite{VK_SIM_LOST_NONE};
void (*gVkDeviceLostHandler)(const char* call, VkResult result, const char* queue,
                             const char* file, int line) = nullptr;
void (*gVkFatalHandler)(const char* expr, VkResult result, const char* file, int line) = nullptr;

// ---- ASYNC COMPUTE command-buffer registry + barrier sanitizer (phase 3a) ----
// The renderer registers the command buffers it records for the async compute queue
// (vkAsyncRegisterCmd). Breadcrumbs use it to keep the two queues' AMD markers apart;
// vkqCmdPipelineBarrier2 uses it to sanitize barriers recorded for a compute-only family.
namespace
{
struct AsyncCmdEntry { std::atomic<VkCommandBuffer> cmd{VK_NULL_HANDLE}; std::atomic<bool> computeOnly{false};
                       std::atomic<const char*> label{nullptr}; };   // label: #845 / #917 (vkAsyncSetCmdLabel)
constexpr uint32_t kAsyncCmdMax = 32;
AsyncCmdEntry sAsyncCmds[kAsyncCmdMax];
std::atomic<uint32_t> sAsyncCmdCount{0};
std::atomic<uint32_t> sAsyncSanitized{0};

const AsyncCmdEntry* asyncEntry(VkCommandBuffer cmd)
{
    if (!cmd || sAsyncCmdCount.load(std::memory_order_relaxed) == 0) return nullptr;
    for (const AsyncCmdEntry& e : sAsyncCmds)
        if (e.cmd.load(std::memory_order_relaxed) == cmd) return &e;
    return nullptr;
}
}

void vkAsyncRegisterCmd(VkCommandBuffer cmd, bool computeOnly)
{
    if (!cmd) return;
    for (AsyncCmdEntry& e : sAsyncCmds)
        if (e.cmd.load() == cmd) { e.computeOnly.store(computeOnly); return; }
    for (AsyncCmdEntry& e : sAsyncCmds)
    {
        VkCommandBuffer expect = VK_NULL_HANDLE;
        if (e.cmd.compare_exchange_strong(expect, cmd))
        {
            e.computeOnly.store(computeOnly);
            sAsyncCmdCount.fetch_add(1);
            return;
        }
    }
    std::fprintf(stderr, "[vkgfx] vkAsyncRegisterCmd: registry full (%u)\n", kAsyncCmdMax);
}

void vkAsyncUnregisterCmd(VkCommandBuffer cmd)
{
    if (!cmd) return;
    for (AsyncCmdEntry& e : sAsyncCmds)
    {
        VkCommandBuffer expect = cmd;
        if (e.cmd.compare_exchange_strong(expect, VK_NULL_HANDLE)) { sAsyncCmdCount.fetch_sub(1); return; }
    }
}

bool vkAsyncIsAsyncCmd(VkCommandBuffer cmd) { return asyncEntry(cmd) != nullptr; }
void vkAsyncSetCmdLabel(VkCommandBuffer cmd, const char* label)
{
    if (const AsyncCmdEntry* e = asyncEntry(cmd)) const_cast<AsyncCmdEntry*>(e)->label.store(label, std::memory_order_relaxed);
}
const char* vkAsyncCmdLabel(VkCommandBuffer cmd)
{
    const AsyncCmdEntry* e = asyncEntry(cmd);
    return e ? e->label.load(std::memory_order_relaxed) : nullptr;
}
bool vkAsyncIsComputeOnlyCmd(VkCommandBuffer cmd)
{
    const AsyncCmdEntry* e = asyncEntry(cmd);
    return e && e->computeOnly.load(std::memory_order_relaxed);
}
uint32_t vkAsyncSanitizedBarriers() { return sAsyncSanitized.load(std::memory_order_relaxed); }
// #845 / #917: image layout transitions recorded into compute-only command buffers that
// involve an ATTACHMENT layout (depth / stencil / colour attachment, or any layout change on
// a depth / stencil aspect). Legal Vulkan, but the transition then runs on a queue without a
// graphics engine (depth / colour compression metadata handled by a compute-queue path that
// D3D12 does not even allow); the renderer records these on the graphics queue around its
// forks (RenderVKAsyncLayoutsOnGraphics). Counted always; each distinct kind logged once.
std::atomic<uint32_t> sAsyncAttachTransitions{0};
uint32_t vkAsyncAttachTransitions() { return sAsyncAttachTransitions.load(std::memory_order_relaxed); }

namespace
{
// Stages a queue WITHOUT the graphics bit supports (sync2). VK_PIPELINE_STAGE_2_TRANSFER_BIT
// (= ALL_TRANSFER) is supported there; the explicit BLIT / RESOLVE bits are graphics-only.
constexpr VkPipelineStageFlags2 kComputeQueueStages =
    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
  | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
  | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT
  | VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
  | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR
  | VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
// Accesses only a graphics stage can perform.
constexpr VkAccessFlags2 kGraphicsOnlyAccess =
    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT
  | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
  | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
  | VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR | VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT
  | VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT
  | VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT;

VkAccessFlags2 accessForStages(VkPipelineStageFlags2 s)
{
    if (s & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) return ~kGraphicsOnlyAccess;
    VkAccessFlags2 a = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    const VkAccessFlags2 kShader = VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT
        | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT
        | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
        | VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
    const VkAccessFlags2 kXfer = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    const VkAccessFlags2 kAs = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    if (s & (VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR)) a |= kShader;
    if (s & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) a |= VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR;
    if (s & VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT) a |= VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    if (s & (VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT)) a |= kXfer;
    if (s & VK_PIPELINE_STAGE_2_HOST_BIT) a |= VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_HOST_WRITE_BIT;
    if (s & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR)
        a |= kAs | kXfer | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
    if (s & VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR) a |= kAs | kXfer;
    if (s & VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT)
        a |= VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_SHADER_READ_BIT;
    return a;
}

// One side (src or dst) of a barrier recorded for a compute-only queue. Graphics stages
// are dropped: their producers ran on the graphics queue BEFORE the fork (the segment's
// semaphore wait, ALL_COMMANDS, orders and publishes them) and their consumers run after
// the join (the graphics wait on this segment's signal does). A side left empty becomes
// COMPUTE_SHADER, never NONE: with NONE a layout transition would not chain after the
// semaphore wait (src) or into the signal (dst). Accesses are clamped to the stages left.
bool sanitizeSide(VkPipelineStageFlags2& st, VkAccessFlags2& ac)
{
    const VkPipelineStageFlags2 in = st;
    const VkAccessFlags2 ain = ac;
    if (st != VK_PIPELINE_STAGE_2_NONE)
    {
        st &= kComputeQueueStages;
        if (st == VK_PIPELINE_STAGE_2_NONE) st = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    ac &= accessForStages(st);
    if (st == VK_PIPELINE_STAGE_2_NONE) ac = 0;
    return st != in || ac != ain;
}
}

// ---- #699 GPU wait accounting + redundant-idle elision (vk_common.h) ----
namespace {
std::atomic<uint64_t> sSubmitSeq{0};
// The submit sequence a completed device idle covered (~0: none yet). A device idle that
// starts while sSubmitSeq still equals it has nothing new to wait for.
std::atomic<uint64_t> sIdleSeq{~0ull};
struct WaitKey
{
    const char* file; int line; const char* kind;
    bool operator==(const WaitKey& o) const { return line == o.line && file == o.file && kind == o.kind; }
};
struct WaitKeyHash
{
    size_t operator()(const WaitKey& k) const
    { return std::hash<const void*>()(k.file) ^ ((size_t)k.line * 0x9E3779B97F4A7C15ull) ^ std::hash<const void*>()(k.kind); }
};
std::mutex sWaitMutex;
std::unordered_map<WaitKey, VkWaitSiteStat, WaitKeyHash> sWaitSites;
VkWaitFrameStat sWaitFrame;
inline double vkNowMs()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void vkWaitNote(const char* kind, const char* file, int line, double ms, bool elided)
{
    std::lock_guard<std::mutex> lk(sWaitMutex);
    VkWaitSiteStat& st = sWaitSites[WaitKey{file, line, kind}];
    st.file = file; st.line = line; st.kind = kind;
    if (elided)
    {
        ++st.elided;
        ++sWaitFrame.elided;
        return;
    }
    ++st.count;
    st.totalMs += ms;
    st.maxMs = std::max(st.maxMs, ms);
    ++sWaitFrame.waits;
    sWaitFrame.ms += ms;
    if (ms > sWaitFrame.worstMs) { sWaitFrame.worstMs = ms; sWaitFrame.worstFile = file; sWaitFrame.worstLine = line; }
}
} // namespace

uint64_t vkSubmitSeq() { return sSubmitSeq.load(std::memory_order_acquire); }

void vkWaitSiteStats(std::vector<VkWaitSiteStat>& out)
{
    out.clear();
    {
        std::lock_guard<std::mutex> lk(sWaitMutex);
        out.reserve(sWaitSites.size());
        for (const auto& kv : sWaitSites) out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const VkWaitSiteStat& a, const VkWaitSiteStat& b) { return a.totalMs > b.totalMs; });
}

VkWaitFrameStat vkWaitFrameTake()
{
    std::lock_guard<std::mutex> lk(sWaitMutex);
    const VkWaitFrameStat r = sWaitFrame;
    sWaitFrame = VkWaitFrameStat{};
    return r;
}

// ---- #697 low-memory accounting (vk_common.h) ----
namespace {
std::atomic<uint64_t> sLowMemRetried[VKLM_COUNT];
std::atomic<uint64_t> sLowMemDropped[VKLM_COUNT];
std::atomic<uint64_t> sLowMemBytes[VKLM_COUNT];
std::atomic<uint32_t> sInjectOomEvery{0};
std::atomic<uint64_t> sInjectOomCount{0};
} // namespace

const char* vkLowMemKindName(uint32_t k)
{
    static const char* const kNames[VKLM_COUNT] = { "staging", "table", "target", "texture", "mesh", "rt",
                                                    "pipeline", "frame", "descriptor", "view" };
    return k < VKLM_COUNT ? kNames[k] : "?";
}

void vkLowMemNote(VkLowMemKind k, bool dropped, uint64_t bytes)
{
    if (k >= VKLM_COUNT) return;
    (dropped ? sLowMemDropped : sLowMemRetried)[k].fetch_add(1, std::memory_order_relaxed);
    sLowMemBytes[k].fetch_add(bytes, std::memory_order_relaxed);
}

VkLowMemStats vkLowMemStats()
{
    VkLowMemStats s;
    for (uint32_t k = 0; k < VKLM_COUNT; ++k)
    {
        s.retried[k] = sLowMemRetried[k].load(std::memory_order_relaxed);
        s.dropped[k] = sLowMemDropped[k].load(std::memory_order_relaxed);
        s.bytes[k] = sLowMemBytes[k].load(std::memory_order_relaxed);
    }
    return s;
}

std::string vkLowMemSummary()
{
    const VkLowMemStats s = vkLowMemStats();
    std::string o;
    for (uint32_t k = 0; k < VKLM_COUNT; ++k)
    {
        if (!s.retried[k] && !s.dropped[k]) continue;
        if (!o.empty()) o += ", ";
        o += vkLowMemKindName(k);
        o += " r" + std::to_string(s.retried[k]) + "/d" + std::to_string(s.dropped[k]);
    }
    return o.empty() ? std::string("none") : o;
}

void vkSetInjectOomCalls(uint32_t every) { sInjectOomEvery.store(every, std::memory_order_relaxed); }

bool vkInjectOomTake()
{
    const uint32_t every = sInjectOomEvery.load(std::memory_order_relaxed);
    if (!every) return false;
    return (sInjectOomCount.fetch_add(1, std::memory_order_relaxed) + 1) % every == 0;
}

#undef vkDeviceWaitIdle   // (vk_common.h routes every caller here)
VkResult vkqDeviceWaitIdle(VkDevice dev, const char* file, int line)
{
    // Lost: never wait on the dead device (some drivers hang there), e.g. in teardown.
    if (vkDeviceIsLost() || !dev) return vkDeviceIsLost() ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    // #699: nothing submitted on any queue since the last completed idle -> the device is
    // still idle (every submit goes through vkqQueueSubmit[2], which bumps the sequence
    // BEFORE calling the driver). Read before the wait: a submit racing the wait leaves
    // the sequence ahead of what this idle records, so the next caller waits again.
    const uint64_t seq = sSubmitSeq.load(std::memory_order_acquire);
    if (sIdleSeq.load(std::memory_order_acquire) == seq)
    {
        vkWaitNote("deviceIdle", file, line, 0.0, true);
        return VK_SUCCESS;
    }
    const double t0 = vkNowMs();
    const VkResult r = vkDeviceWaitIdle(dev);
    vkWaitNote("deviceIdle", file, line, vkNowMs() - t0, false);
    if (r == VK_SUCCESS) sIdleSeq.store(seq, std::memory_order_release);
    if (r == VK_ERROR_DEVICE_LOST)
    {
        std::fprintf(stderr, "[vktest] vkDeviceWaitIdle = VK_ERROR_DEVICE_LOST at %s:%d\n", file, line);
        vkDeviceLostRaise("vkDeviceWaitIdle", r, nullptr, file, line);
    }
    return r;
}

namespace {
bool isAttachmentLayout(VkImageLayout l)
{
    switch (l)
    {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        return true;
    default:
        return false;
    }
}
// (see sAsyncAttachTransitions) One log line per distinct (old, new, aspect).
void noteComputeQueueAttachTransition(VkCommandBuffer cmd, const VkImageMemoryBarrier2& b)
{
    if (b.oldLayout == b.newLayout) return;
    const bool ds = (b.subresourceRange.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    if (!ds && !isAttachmentLayout(b.oldLayout) && !isAttachmentLayout(b.newLayout)) return;
    sAsyncAttachTransitions.fetch_add(1, std::memory_order_relaxed);
    static std::mutex s_m;
    static std::unordered_set<uint64_t> s_seen;
    // One line per distinct (old, new, aspect, segment, image name).
    const char* seg = vkAsyncCmdLabel(cmd);
    char img[32] = "";
    vkImgNameOf(b.image, img, sizeof(img));
    uint64_t key = ((uint64_t)(uint32_t)b.oldLayout << 40) ^ ((uint64_t)(uint32_t)b.newLayout << 8)
                 ^ (uint64_t)b.subresourceRange.aspectMask ^ ((uint64_t)(uintptr_t)seg * 0x9E3779B97F4A7C15ull);
    for (const char* c = img; *c; ++c) key = key * 131u + (uint8_t)*c;
    {
        std::lock_guard<std::mutex> lk(s_m);
        if (s_seen.size() >= 64 || !s_seen.insert(key).second) return;
    }
    char line[320];
    std::snprintf(line, sizeof(line), "[vkgfx] compute-only queue: image layout transition %d -> %d (aspect 0x%x, "
                  "layers %u+%u) of image '%s' (0x%llx) recorded in async segment %s (#845 / #917 diagnostic; "
                  "logged once per kind)",
                  (int)b.oldLayout, (int)b.newLayout, (unsigned)b.subresourceRange.aspectMask,
                  b.subresourceRange.baseArrayLayer, b.subresourceRange.layerCount,
                  img[0] ? img : "?", (unsigned long long)(uintptr_t)b.image, seg ? seg : "(unlabelled)");
    std::fprintf(stderr, "%s\n", line);
    if (gVkFaultLogSink) gVkFaultLogSink(line);
}
} // namespace

#undef vkCmdPipelineBarrier2   // (vk_common.h routes every caller here)
void vkqCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dep)
{
    if (!dep || !vkAsyncIsComputeOnlyCmd(cmd))
    {
        gVkCompat.CmdPipelineBarrier2(cmd, dep);   // #675: core on 1.3, the KHR entry point on 1.2
        return;
    }
    // Copies: the caller's arrays are const (and often reused after the call).
    static thread_local std::vector<VkMemoryBarrier2> mb;
    static thread_local std::vector<VkBufferMemoryBarrier2> bb;
    static thread_local std::vector<VkImageMemoryBarrier2> ib;
    mb.assign(dep->pMemoryBarriers, dep->pMemoryBarriers + dep->memoryBarrierCount);
    bb.assign(dep->pBufferMemoryBarriers, dep->pBufferMemoryBarriers + dep->bufferMemoryBarrierCount);
    ib.assign(dep->pImageMemoryBarriers, dep->pImageMemoryBarriers + dep->imageMemoryBarrierCount);
    bool changed = false;
    for (VkMemoryBarrier2& b : mb)
    {
        changed |= sanitizeSide(b.srcStageMask, b.srcAccessMask);
        changed |= sanitizeSide(b.dstStageMask, b.dstAccessMask);
    }
    for (VkBufferMemoryBarrier2& b : bb)
    {
        changed |= sanitizeSide(b.srcStageMask, b.srcAccessMask);
        changed |= sanitizeSide(b.dstStageMask, b.dstAccessMask);
    }
    for (VkImageMemoryBarrier2& b : ib)
    {
        noteComputeQueueAttachTransition(cmd, b);
        changed |= sanitizeSide(b.srcStageMask, b.srcAccessMask);
        changed |= sanitizeSide(b.dstStageMask, b.dstAccessMask);
    }
    if (changed) sAsyncSanitized.fetch_add(1, std::memory_order_relaxed);
    VkDependencyInfo d = *dep;
    d.pMemoryBarriers = mb.data();
    d.pBufferMemoryBarriers = bb.data();
    d.pImageMemoryBarriers = ib.data();
    gVkCompat.CmdPipelineBarrier2(cmd, &d);
}

void vkBreadcrumb(VkCommandBuffer cmd, const char* name)
{
    {
        const uint32_t m = sMarkSeq.fetch_add(1, std::memory_order_relaxed);
        sMarkRing[m % MARK_RING].store(name, std::memory_order_relaxed);
    }
    if (sCmdSetCheckpointNV)
    {
        sCmdSetCheckpointNV(cmd, name);   // per queue: vkReportBreadcrumbs reads both
    }
    if (sCmdInsertLabel)
    {
        // #973: the same name as a debug-utils label (capture / crash tools). Recording-time
        // only: no GPU work on drivers without a tool attached.
        VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        l.pLabelName = name;
        sCmdInsertLabel(cmd, &l);
    }
    if (sCmdWriteBufferMarkerAMD && sCrumbBuf.buffer)
    {
        // Async compute command buffers mark words 2 / 3, the graphics queue 0 / 1.
        const VkDeviceSize base = vkAsyncIsAsyncCmd(cmd) ? 8 : 0;
        const uint32_t seq = ++sCrumbSeq;
        sCrumbNames[seq % CRUMB_RING] = name;
        sCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sCrumbBuf.buffer, base, seq);
        sCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sCrumbBuf.buffer, base + 4, seq);
    }
}

// ---- DEVICE_LOST report output: stderr + the viewer's log (gVkFaultLogSink) ----
void (*gVkFaultLogSink)(const char* line) = nullptr;
static std::string sFaultLine;   // partial line (some reports print a line in pieces)
static void vkFaultFlushLine()
{
    if (!sFaultLine.empty() && gVkFaultLogSink) gVkFaultLogSink(sFaultLine.c_str());
    sFaultLine.clear();
}
static void vkFaultLog(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stderr);
    for (const char* c = buf; *c; ++c)
    {
        if (*c == '\n') vkFaultFlushLine();
        else sFaultLine.push_back(*c);
    }
}

static void vkReportBreadcrumbs()
{
    const VkQueue queues[2] = {sCrumbQueue, sCrumbQueue2};
    const char* qname[2] = {"graphics queue", "async compute queue"};
    // vkGetQueueCheckpointDataNV requires a LOST device (VUID-vkGetQueueCheckpointDataNV-
    // queue-02025): skipped for a simulated loss.
    const bool confirmed = sLostConfirmed.load(std::memory_order_acquire);
    if (sGetQueueCheckpointDataNV && !confirmed)
        vkFaultLog("[vktest] DEVICE_LOST breadcrumbs (NV checkpoints): %s, query skipped\n",
                   vkDeviceLostSimulated() ? "simulated loss" : "loss not confirmed by the driver");
    for (int q = 0; q < 2; ++q)
    {
        if (!sGetQueueCheckpointDataNV || !queues[q] || !confirmed) continue;
        uint32_t n = 0;
        sGetQueueCheckpointDataNV(queues[q], &n, nullptr);
        std::vector<VkCheckpointDataNV> cps(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (n) sGetQueueCheckpointDataNV(queues[q], &n, cps.data());
        vkFaultLog("[vktest] DEVICE_LOST breadcrumbs (NV checkpoints, %s): %u\n", qname[q], n);
        for (const auto& c : cps)
            vkFaultLog("[vktest]   stage=0x%x last=%s\n", (unsigned)c.stage,
                         c.pCheckpointMarker ? (const char*)c.pCheckpointMarker : "?");
    }
    if (sCrumbBuf.mapped)
    {
        const uint32_t* m = (const uint32_t*)sCrumbBuf.mapped;
        auto nameOf = [](uint32_t s) -> const char* {
            if (!s || s + CRUMB_RING <= sCrumbSeq) return "?";
            const char* n = sCrumbNames[s % CRUMB_RING];
            return n ? n : "?";
        };
        for (int q = 0; q < (sCrumbQueue2 ? 2 : 1); ++q)
        {
            const uint32_t started = m[2 * q], finished = m[2 * q + 1];
            vkFaultLog("[vktest] DEVICE_LOST breadcrumbs (AMD markers, %s): last started #%u %s, "
                                 "last finished #%u %s (latest recorded #%u)\n",
                         qname[q], started, nameOf(started), finished, nameOf(finished), sCrumbSeq);
        }
    }
    std::fflush(stderr);
}


// ---- GPU address registry (DEVICE_LOST forensics; see vk_common.h) ----
namespace
{
struct AddrRange
{
    VkDeviceAddress lo = 0;
    VkDeviceSize size = 0;
    uint32_t usage = 0;         // buffer usage flags (buffers) / id (noted ranges)
    uint32_t frame = 0;         // created
    uint32_t freedFrame = 0;    // destroyed (ring entries)
    const void* caller = nullptr;
    char name[24] = {};
    bool noted = false;         // AS sub-range, not a VkBuffer
};
// An image (VkDeviceContext::createImage*): its memory's GPU VA range when known
// (lo = 0 otherwise), extent, debug name, created / freed frame.
struct ImgRange
{
    VkDeviceAddress lo = 0;
    VkDeviceSize size = 0;
    uint64_t image = 0;         // VkImage handle value (may be recycled: serial is unique)
    uint64_t serial = 0;        // never reused
    uint32_t w = 0, h = 0, layers = 0;
    uint32_t fmt = 0;
    uint32_t frame = 0;         // created
    uint32_t freedFrame = 0;    // destroyed (ring entries)
    const void* caller = nullptr;
    char name[24] = {};
};
// Descriptor tracking: one single image descriptor as written.
struct DescEntry
{
    uint32_t binding = 0;
    uint64_t view = 0;          // VkImageView handle value
    uint64_t viewSerial = 0;    // the view's serial when written
    uint64_t imgSerial = 0;     // its image's serial (0 = not a registry image)
};
struct AddrRegistry
{
    std::mutex m;
    std::map<VkDeviceAddress, AddrRange> live;           // buffers, by start
    std::unordered_map<uint64_t, VkDeviceAddress> byBuf;  // VkBuffer -> start
    std::map<VkDeviceAddress, AddrRange> notes;          // AS ranges, by start
    static constexpr size_t kRing = 65536;   // #917: longer -- frees that may feed async work outlive several frames
    std::vector<AddrRange> freed = std::vector<AddrRange>(kRing);
    size_t freedHead = 0, freedCount = 0;
    uint32_t frame = 0;
    // ---- images (vkImgTrack) ----
    std::unordered_map<uint64_t, ImgRange> imgLive;      // VkImage -> record
    std::map<VkDeviceAddress, uint64_t> imgByVa;         // live images with a known VA -> VkImage
    static constexpr size_t kImgRing = 16384;   // #917: texture churn emptied 4096 in a few dozen frames
    std::vector<ImgRange> imgFreed = std::vector<ImgRange>(kImgRing);
    size_t imgFreedHead = 0, imgFreedCount = 0;
    uint64_t nextImgSerial = 1;
    // ---- memory blocks (#845 / #917, vkMemBlockTrack): texture-pool / alias-arena blocks ----
    std::map<VkDeviceAddress, AddrRange> blkLive;            // by VA start
    std::unordered_map<uint64_t, VkDeviceAddress> blkByMem;  // VkDeviceMemory -> start
    static constexpr size_t kBlkRing = 8192;
    std::vector<AddrRange> blkFreed = std::vector<AddrRange>(kBlkRing);
    size_t blkFreedHead = 0, blkFreedCount = 0;
    // ---- views (vkViewTrack): handle -> {serial, image handle, image serial} ----
    struct ViewRec { uint64_t serial = 0; uint64_t image = 0; uint64_t imgSerial = 0; };
    std::unordered_map<uint64_t, ViewRec> views;
    uint64_t nextViewSerial = 1;
    // ---- descriptor tracking (vkDescTrack*) ----
    std::unordered_map<uint64_t, std::vector<DescEntry>> descSets;   // VkDescriptorSet -> entries
    std::unordered_set<uint64_t> descReported;                       // (set, binding, view serial) logged
    uint32_t descStale = 0;
    void pushImgFreed(ImgRange r)
    {
        r.freedFrame = frame;
        imgFreed[imgFreedHead] = r;
        imgFreedHead = (imgFreedHead + 1) % kImgRing;
        if (imgFreedCount < kImgRing) ++imgFreedCount;
    }
    const ImgRange* findImgBySerial(uint64_t serial) const
    {
        if (!serial) return nullptr;
        for (const auto& kv : imgLive) if (kv.second.serial == serial) return &kv.second;
        for (size_t i = 0; i < imgFreedCount; ++i)
        {
            const ImgRange& r = imgFreed[(imgFreedHead + kImgRing - 1 - i) % kImgRing];
            if (r.serial == serial) return &r;
        }
        return nullptr;
    }
    void pushFreed(AddrRange r)
    {
        r.freedFrame = frame;
        freed[freedHead] = r;
        freedHead = (freedHead + 1) % kRing;
        if (freedCount < kRing) ++freedCount;
    }
};
AddrRegistry& addrReg() { static AddrRegistry* r = new AddrRegistry; return *r; }   // leaked: usable at exit

void describeRange(const char* tag, const AddrRange& r, uint32_t now)
{
    const char* mod = "?";
    unsigned long long off = (unsigned long long)(uintptr_t)r.caller;
#if LL_WINDOWS
    // Module base + name of the allocating caller (dladdr equivalent).
    static char modname[MAX_PATH];
    HMODULE hm = nullptr;
    if (r.caller && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)r.caller, &hm) && hm)
    {
        off = (unsigned long long)((uintptr_t)r.caller - (uintptr_t)hm);
        if (GetModuleFileNameA(hm, modname, MAX_PATH))
            mod = std::strrchr(modname, '\\') ? std::strrchr(modname, '\\') + 1 : modname;
    }
#else
    Dl_info di{};
    if (r.caller && dladdr(r.caller, &di) && di.dli_fbase)
    {
        off = (unsigned long long)((uintptr_t)r.caller - (uintptr_t)di.dli_fbase);
        mod = di.dli_fname ? std::strrchr(di.dli_fname, '/') ? std::strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?";
    }
#endif
    if (r.noted)
        vkFaultLog("[vktest]     %s AS %s id=%u [0x%llx, 0x%llx) size=%llu created frame %u (now %u)%s",
                     tag, r.name, r.usage, (unsigned long long)r.lo, (unsigned long long)(r.lo + r.size),
                     (unsigned long long)r.size, r.frame, now, r.freedFrame ? "" : "\n");
    else
        vkFaultLog("[vktest]     %s buffer '%s' usage=0x%x [0x%llx, 0x%llx) size=%llu created frame %u by %s+0x%llx (now %u)%s",
                     tag, r.name[0] ? r.name : "-", r.usage, (unsigned long long)r.lo,
                     (unsigned long long)(r.lo + r.size), (unsigned long long)r.size, r.frame, mod, off, now,
                     r.freedFrame ? "" : "\n");
    if (r.freedFrame) vkFaultLog(" FREED frame %u\n", r.freedFrame);
}

const char* callerModule(const void* caller, unsigned long long& off)
{
    const char* mod = "?";
    off = (unsigned long long)(uintptr_t)caller;
#if LL_WINDOWS
    static char modname[MAX_PATH];
    HMODULE hm = nullptr;
    if (caller && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCSTR)caller, &hm) && hm)
    {
        off = (unsigned long long)((uintptr_t)caller - (uintptr_t)hm);
        if (GetModuleFileNameA(hm, modname, MAX_PATH))
            mod = std::strrchr(modname, '\\') ? std::strrchr(modname, '\\') + 1 : modname;
    }
#else
    Dl_info di{};
    if (caller && dladdr(caller, &di) && di.dli_fbase)
    {
        off = (unsigned long long)((uintptr_t)caller - (uintptr_t)di.dli_fbase);
        mod = di.dli_fname ? std::strrchr(di.dli_fname, '/') ? std::strrchr(di.dli_fname, '/') + 1 : di.dli_fname : "?";
    }
#endif
    return mod;
}

void describeImage(const char* tag, const ImgRange& r, uint32_t now)
{
    unsigned long long off = 0;
    const char* mod = callerModule(r.caller, off);
    char va[64] = "VA unknown";
    if (r.lo) std::snprintf(va, sizeof(va), "[0x%llx, 0x%llx)", (unsigned long long)r.lo,
                            (unsigned long long)(r.lo + r.size));
    vkFaultLog("[vktest]     %s image '%s' %ux%u%s%s fmt=%u %s size=%llu created frame %u by %s+0x%llx (now %u)%s",
               tag, r.name[0] ? r.name : "-", r.w, r.h, r.layers > 1 ? " x" : "",
               r.layers > 1 ? std::to_string(r.layers).c_str() : "", r.fmt, va, (unsigned long long)r.size,
               r.frame, mod, off, now, r.freedFrame ? "" : "\n");
    if (r.freedFrame) vkFaultLog(" FREED frame %u\n", r.freedFrame);
}

void mapFaultAddress(VkDeviceAddress addr, VkDeviceSize precision)
{
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::try_to_lock);   // never deadlock the crash path
    if (!lk.owns_lock()) { vkFaultLog("[vktest]     (address registry busy)\n"); return; }
    const VkDeviceSize p = precision ? precision : 1;
    const VkDeviceAddress lo = addr & ~(VkDeviceAddress)(p - 1), hi = lo + p;
    auto overlaps = [&](const AddrRange& r) { return r.lo < hi && lo < r.lo + r.size; };
    int hits = 0;
    for (const auto* m : {&R.live, &R.notes})
    {
        auto it = m->upper_bound(hi);
        while (it != m->begin())
        {
            --it;
            if (overlaps(it->second)) { describeRange("LIVE ", it->second, R.frame); ++hits; }
            if (it->second.lo + (1ull << 32) < lo) break;   // ranges are < 4 GB
        }
    }
    int freedHits = 0;
    for (size_t i = 0; i < R.freedCount && freedHits < 12; ++i)
    {
        const AddrRange& r = R.freed[(R.freedHead + AddrRegistry::kRing - 1 - i) % AddrRegistry::kRing];
        if (overlaps(r)) { describeRange(hits ? "prev-freed range (under the LIVE object above):" : "FREED", r, R.frame); ++freedHits; }
    }
    // Whole memory blocks (#845 / #917): texture-pool / alias-arena blocks, live and freed
    // (the freed ring is long: a block outlives the image records that were in it).
    {
        auto it = R.blkLive.upper_bound(hi);
        while (it != R.blkLive.begin())
        {
            --it;
            if (overlaps(it->second))
            {
                vkFaultLog("[vktest]     LIVE  memory block '%s' [0x%llx, 0x%llx) size=%llu created frame %u (now %u)\n",
                           it->second.name, (unsigned long long)it->second.lo,
                           (unsigned long long)(it->second.lo + it->second.size), (unsigned long long)it->second.size,
                           it->second.frame, R.frame);
                ++hits;
            }
            if (it->second.lo + (1ull << 32) < lo) break;
        }
        int blkHits = 0;
        for (size_t i = 0; i < R.blkFreedCount && blkHits < 8; ++i)
        {
            const AddrRange& r = R.blkFreed[(R.blkFreedHead + AddrRegistry::kBlkRing - 1 - i) % AddrRegistry::kBlkRing];
            if (!overlaps(r)) continue;
            vkFaultLog("[vktest]     FREED memory block '%s' [0x%llx, 0x%llx) size=%llu created frame %u, FREED frame %u "
                       "(%u frames ago)\n", r.name, (unsigned long long)r.lo, (unsigned long long)(r.lo + r.size),
                       (unsigned long long)r.size, r.frame, r.freedFrame, R.frame - r.freedFrame);
            ++blkHits;
            ++freedHits;
        }
    }
    // Images (their memory's VA range, where the alias probe got one).
    {
        auto it = R.imgByVa.upper_bound(hi);
        while (it != R.imgByVa.begin())
        {
            --it;
            auto lv = R.imgLive.find(it->second);
            if (lv != R.imgLive.end() && lv->second.lo < hi && lo < lv->second.lo + lv->second.size)
                { describeImage("LIVE ", lv->second, R.frame); ++hits; }
            if (it->first + (1ull << 32) < lo) break;
        }
        for (size_t i = 0; i < R.imgFreedCount && freedHits < 24; ++i)
        {
            const ImgRange& r = R.imgFreed[(R.imgFreedHead + AddrRegistry::kImgRing - 1 - i) % AddrRegistry::kImgRing];
            if (r.lo && r.lo < hi && lo < r.lo + r.size) { describeImage("FREED", r, R.frame); ++freedHits; }
        }
    }
    if (hits + freedHits == 0)
    {
        vkFaultLog("[vktest]     no live/recently-freed buffer, AS or image contains it; neighbours:\n");
        auto up = R.live.lower_bound(lo);
        if (up != R.live.end()) describeRange("above", up->second, R.frame);
        if (up != R.live.begin()) describeRange("below", std::prev(up)->second, R.frame);
        // #874: the AS notes too (an AS is a sub-range of its storage buffer: the buffer
        // neighbours above say nothing about which BLAS sits nearest), and how far past the
        // top of every live allocation the address lies.
        auto nup = R.notes.lower_bound(lo);
        if (nup != R.notes.end()) describeRange("above", nup->second, R.frame);
        if (nup != R.notes.begin()) describeRange("below", std::prev(nup)->second, R.frame);
        VkDeviceAddress top = 0;
        if (!R.live.empty()) top = std::max<VkDeviceAddress>(top, R.live.rbegin()->second.lo + R.live.rbegin()->second.size);
        if (!R.notes.empty()) top = std::max<VkDeviceAddress>(top, R.notes.rbegin()->second.lo + R.notes.rbegin()->second.size);
        if (top && lo >= top)
            vkFaultLog("[vktest]     the address is %.1f MB above the end of the highest live buffer / AS (0x%llx)\n",
                       (double)(lo - top) / (1024.0 * 1024.0), (unsigned long long)top);
        // #917: a linear read running off the END of the range just below lands at an address
        // nothing contains (0x5e00000 was 2 MB-aligned: an allocation boundary). The nearest
        // range ending at or below the address, live or freed, of every kind, within 64 MB.
        {
            const VkDeviceAddress win = 64ull << 20;
            const AddrRange* best = nullptr; const char* bestKind = ""; bool bestFreed = false;
            const ImgRange* bestImg = nullptr; bool bestImgFreed = false;
            VkDeviceAddress bestEnd = 0;
            auto consider = [&](const AddrRange& r, const char* kind, bool freed)
            {
                const VkDeviceAddress e = r.lo + r.size;
                if (!r.lo || e > lo || lo - e > win || ((best || bestImg) && e <= bestEnd)) return;
                best = &r; bestKind = kind; bestFreed = freed; bestImg = nullptr; bestEnd = e;
            };
            for (const auto& kv : R.live) consider(kv.second, "buffer", false);
            for (const auto& kv : R.notes) consider(kv.second, "AS", false);
            for (const auto& kv : R.blkLive) consider(kv.second, "memory block", false);
            for (size_t i = 0; i < R.freedCount; ++i)
                consider(R.freed[(R.freedHead + AddrRegistry::kRing - 1 - i) % AddrRegistry::kRing], "buffer/AS", true);
            for (size_t i = 0; i < R.blkFreedCount; ++i)
                consider(R.blkFreed[(R.blkFreedHead + AddrRegistry::kBlkRing - 1 - i) % AddrRegistry::kBlkRing],
                         "memory block", true);
            auto considerImg = [&](const ImgRange& r, bool freed)
            {
                const VkDeviceAddress e = r.lo + r.size;
                if (!r.lo || e > lo || lo - e > win || ((best || bestImg) && e <= bestEnd)) return;
                bestImg = &r; bestImgFreed = freed; best = nullptr; bestEnd = e;
            };
            for (const auto& kv : R.imgLive) considerImg(kv.second, false);
            for (size_t i = 0; i < R.imgFreedCount; ++i)
                considerImg(R.imgFreed[(R.imgFreedHead + AddrRegistry::kImgRing - 1 - i) % AddrRegistry::kImgRing], true);
            if (best || bestImg)
                vkFaultLog("[vktest]     nearest range ENDING at or below it (%s, %llu bytes below the address):\n",
                           best ? (bestFreed ? "freed" : "live") : (bestImgFreed ? "freed" : "live"),
                           (unsigned long long)(lo - bestEnd));
            if (best) { vkFaultLog("[vktest]     (%s)\n", bestKind); describeRange(bestFreed ? "FREED" : "LIVE ", *best, R.frame); }
            if (bestImg) describeImage(bestImgFreed ? "FREED" : "LIVE ", *bestImg, R.frame);
        }
    }
    vkFaultLog("[vktest]     (registry: %zu live buffers, %zu AS, %zu recent frees, %zu live images "
               "(%zu with a VA), %zu recent image frees, %zu live memory blocks, %zu block frees, frame %u)\n",
                 R.live.size(), R.notes.size(), R.freedCount, R.imgLive.size(), R.imgByVa.size(),
                 R.imgFreedCount, R.blkLive.size(), R.blkFreedCount, R.frame);
    // How far back each ring reaches (a miss is only meaningful within it).
    {
        auto oldest = [](size_t count, size_t head, size_t ring, auto& v) -> uint32_t
        { return count ? v[(head + ring - count) % ring].freedFrame : 0u; };
        vkFaultLog("[vktest]     (free history reaches back to frame: buffers/AS %u, images %u, blocks %u)\n",
                   oldest(R.freedCount, R.freedHead, AddrRegistry::kRing, R.freed),
                   oldest(R.imgFreedCount, R.imgFreedHead, AddrRegistry::kImgRing, R.imgFreed),
                   oldest(R.blkFreedCount, R.blkFreedHead, AddrRegistry::kBlkRing, R.blkFreed));
    }
}

// Every image freed in the last few frames, newest first: with a stale descriptor the
// culprit is almost always one of them, VA match or not.
void listRecentImageFrees()
{
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::try_to_lock);
    if (!lk.owns_lock()) { vkFaultLog("[vktest] (image registry busy)\n"); return; }
    constexpr uint32_t kFrames = 4, kMax = 48;
    uint32_t n = 0, total = 0;
    for (size_t i = 0; i < R.imgFreedCount; ++i)
    {
        const ImgRange& r = R.imgFreed[(R.imgFreedHead + AddrRegistry::kImgRing - 1 - i) % AddrRegistry::kImgRing];
        if (r.freedFrame + kFrames < R.frame) break;   // ring is in free order
        ++total;
        if (n == 0) vkFaultLog("[vktest] images freed in the last %u frames (newest first):\n", kFrames);
        if (n < kMax) { describeImage("freed", r, R.frame); ++n; }
    }
    if (total > n) vkFaultLog("[vktest]     ... %u more\n", total - n);
    if (total == 0) vkFaultLog("[vktest] no image freed in the last %u frames\n", kFrames);
}
} // namespace

void vkAddrTrackBuffer(VkBuffer b, VkDeviceAddress a, VkDeviceSize size, uint32_t usage, const void* caller)
{
    if (!b || !a) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    AddrRange r; r.lo = a; r.size = size; r.usage = usage; r.frame = R.frame; r.caller = caller;
    R.live[a] = r;
    R.byBuf[(uint64_t)(uintptr_t)b] = a;
}

void vkAddrUntrackBuffer(VkBuffer b)
{
    if (!b) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.byBuf.find((uint64_t)(uintptr_t)b);
    if (it == R.byBuf.end()) return;
    auto lv = R.live.find(it->second);
    if (lv != R.live.end()) { R.pushFreed(lv->second); R.live.erase(lv); }
    R.byBuf.erase(it);
}

void vkAddrName(VkBuffer b, const char* name)
{
    if (!b || !name) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.byBuf.find((uint64_t)(uintptr_t)b);
    if (it == R.byBuf.end()) return;
    auto lv = R.live.find(it->second);
    if (lv != R.live.end()) std::snprintf(lv->second.name, sizeof(lv->second.name), "%s", name);
}

void vkAddrNote(VkDeviceAddress a, VkDeviceSize size, const char* kind, uint32_t id)
{
    if (!a) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    AddrRange r; r.lo = a; r.size = size; r.usage = id; r.frame = R.frame; r.noted = true;
    std::snprintf(r.name, sizeof(r.name), "%s", kind ? kind : "?");
    R.notes[a] = r;
}

void vkAddrForget(VkDeviceAddress a)
{
    if (!a) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.notes.find(a);
    if (it == R.notes.end()) return;
    R.pushFreed(it->second);
    R.notes.erase(it);
}

void vkAddrFrameTick()
{
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    ++R.frame;
}

void vkMemBlockTrack(VkDeviceMemory mem, VkDeviceAddress va, VkDeviceSize size, const char* kind)
{
    if (!mem || !va) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    AddrRange r; r.lo = va; r.size = size; r.frame = R.frame;
    std::snprintf(r.name, sizeof(r.name), "%s", kind ? kind : "block");
    R.blkLive[va] = r;
    R.blkByMem[(uint64_t)(uintptr_t)mem] = va;
}

void vkImgNameOf(VkImage image, char* out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if (!image) return;
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::try_to_lock);
    if (!lk.owns_lock()) return;
    auto it = R.imgLive.find((uint64_t)(uintptr_t)image);
    if (it != R.imgLive.end()) std::snprintf(out, n, "%s", it->second.name);
}

void vkMemBlockUntrack(VkDeviceMemory mem)
{
    if (!mem) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.blkByMem.find((uint64_t)(uintptr_t)mem);
    if (it == R.blkByMem.end()) return;
    auto lv = R.blkLive.find(it->second);
    if (lv != R.blkLive.end())
    {
        AddrRange r = lv->second;
        r.freedFrame = R.frame;
        R.blkFreed[R.blkFreedHead] = r;
        R.blkFreedHead = (R.blkFreedHead + 1) % AddrRegistry::kBlkRing;
        if (R.blkFreedCount < AddrRegistry::kBlkRing) ++R.blkFreedCount;
        R.blkLive.erase(lv);
    }
    R.blkByMem.erase(it);
}

uint32_t vkAddrCurrentFrame()
{
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    return R.frame;
}

// ---- registry queries (#874 RT scene audit) ----
namespace
{
// The registry lock for a query: try first (never deadlock a crash path), then a few short
// retries -- another thread may be creating / freeing a buffer right now.
bool lockForQuery(std::unique_lock<std::mutex>& lk)
{
    for (int i = 0; i < 40; ++i)
    {
        if (lk.try_lock()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void fillHit(VkAddrHit& h, const AddrRange& r, VkDeviceAddress x, uint8_t cls)
{
    h.lo = r.lo; h.size = r.size; h.id = r.usage; h.frame = r.frame; h.freedFrame = r.freedFrame;
    h.cls = cls; h.exact = x == r.lo;
    std::memcpy(h.name, r.name, sizeof(h.name));
    h.name[sizeof(h.name) - 1] = 0;
}

const AddrRange* findLiveRange(const std::map<VkDeviceAddress, AddrRange>& m, VkDeviceAddress x)
{
    auto it = m.upper_bound(x);
    if (it == m.begin()) return nullptr;
    --it;
    return x < it->second.lo + it->second.size ? &it->second : nullptr;
}

// The freed ring sorted by start (AS notes and buffers apart) for many lookups.
struct FreedIndex
{
    std::vector<const AddrRange*> v;
    VkDeviceSize maxSize = 0;
    void sort()
    {
        std::sort(v.begin(), v.end(), [](const AddrRange* a, const AddrRange* b) { return a->lo < b->lo; });
    }
    // The newest freed range containing x (nullptr = none).
    const AddrRange* find(VkDeviceAddress x) const
    {
        auto it = std::upper_bound(v.begin(), v.end(), x, [](VkDeviceAddress k, const AddrRange* r) { return k < r->lo; });
        const AddrRange* best = nullptr;
        while (it != v.begin())
        {
            --it;
            const AddrRange* r = *it;
            if (x - r->lo >= maxSize) break;   // every earlier start is farther still
            if (x < r->lo + r->size && (!best || r->freedFrame > best->freedFrame)) best = r;
        }
        return best;
    }
};
} // namespace

bool vkAddrClassify(const VkDeviceAddress* a, size_t n, VkAddrHit* out)
{
    if (!a || !out || !n) return true;
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::defer_lock);
    if (!lockForQuery(lk)) return false;
    FreedIndex fas, fbuf;
    for (size_t i = 0; i < R.freedCount; ++i)
    {
        const AddrRange& r = R.freed[(R.freedHead + AddrRegistry::kRing - 1 - i) % AddrRegistry::kRing];
        if (!r.lo || !r.size) continue;
        FreedIndex& f = r.noted ? fas : fbuf;
        f.v.push_back(&r);
        f.maxSize = std::max(f.maxSize, r.size);
    }
    fas.sort();
    fbuf.sort();
    for (size_t i = 0; i < n; ++i)
    {
        const VkDeviceAddress x = a[i];
        VkAddrHit& h = out[i];
        h = VkAddrHit{};
        if (!x) continue;
        // #973: live first (see VkAddrClass). The freed range a live object reuses is kept
        // as the hit's "previously freed range".
        const AddrRange* fa = fas.find(x);
        const AddrRange* fb = fbuf.find(x);
        auto prev = [&](const AddrRange* r, uint8_t kind)
        {
            if (!r) return false;
            h.prevKind = kind; h.prevId = r->usage; h.prevFreedFrame = r->freedFrame;
            std::memcpy(h.prevName, r->name, sizeof(h.prevName));
            h.prevName[sizeof(h.prevName) - 1] = 0;
            return true;
        };
        if (const AddrRange* r = findLiveRange(R.notes, x))
        {
            fillHit(h, *r, x, VKADDR_AS_LIVE);
            if (!prev(fa, 1)) prev(fb, 2);
            continue;
        }
        const AddrRange* lb = findLiveRange(R.live, x);
        const bool lbAsStorage = lb && (lb->usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR) != 0;
        if (lb && !lbAsStorage)
        {
            fillHit(h, *lb, x, VKADDR_BUF_LIVE);
            if (!prev(fa, 1)) prev(fb, 2);
            continue;
        }
        // AS storage with no live AS here: a freed AS note is the dangling-BLAS evidence.
        if (fa) { fillHit(h, *fa, x, VKADDR_AS_FREED); continue; }
        if (lb) { fillHit(h, *lb, x, VKADDR_BUF_LIVE); prev(fb, 2); continue; }
        if (fb) { fillHit(h, *fb, x, VKADDR_BUF_FREED); continue; }
    }
    return true;
}

bool vkAddrOfBuffer(VkBuffer b, VkDeviceAddress& lo, VkDeviceSize& size)
{
    lo = 0; size = 0;
    if (!b) return false;
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::defer_lock);
    if (!lockForQuery(lk)) return false;
    auto it = R.byBuf.find((uint64_t)(uintptr_t)b);
    if (it == R.byBuf.end()) return false;
    auto lv = R.live.find(it->second);
    if (lv == R.live.end()) return false;
    lo = lv->second.lo; size = lv->second.size;
    return true;
}

bool vkAddrNeighbourAS(VkDeviceAddress a, VkAddrNeighbours& out)
{
    out = VkAddrNeighbours{};
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::defer_lock);
    if (!lockForQuery(lk)) return false;
    auto up = R.notes.upper_bound(a);
    if (up != R.notes.end()) fillHit(out.asAbove, up->second, a, VKADDR_AS_LIVE);
    if (up != R.notes.begin()) fillHit(out.asBelow, std::prev(up)->second, a, VKADDR_AS_LIVE);
    for (size_t i = 0; i < R.freedCount; ++i)
    {
        const AddrRange& r = R.freed[(R.freedHead + AddrRegistry::kRing - 1 - i) % AddrRegistry::kRing];
        if (!r.noted || !r.lo) continue;
        if (r.lo <= a && (out.freedAsBelow.cls == VKADDR_UNKNOWN || r.lo > out.freedAsBelow.lo))
            fillHit(out.freedAsBelow, r, a, VKADDR_AS_FREED);
        if (r.lo > a && (out.freedAsAbove.cls == VKADDR_UNKNOWN || r.lo < out.freedAsAbove.lo))
            fillHit(out.freedAsAbove, r, a, VKADDR_AS_FREED);
    }
    if (!R.live.empty())
        out.liveTop = std::max<VkDeviceAddress>(out.liveTop, R.live.rbegin()->second.lo + R.live.rbegin()->second.size);
    if (!R.notes.empty())
        out.liveTop = std::max<VkDeviceAddress>(out.liveTop, R.notes.rbegin()->second.lo + R.notes.rbegin()->second.size);
    return true;
}

// ---- image registry ----
void vkImgTrack(VkImage img, VkDeviceAddress va, VkDeviceSize size, uint32_t w, uint32_t h,
                uint32_t layers, VkFormat fmt, const void* caller)
{
    if (!img) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    ImgRange r;
    r.lo = va; r.size = size; r.image = (uint64_t)(uintptr_t)img; r.serial = R.nextImgSerial++;
    r.w = w; r.h = h; r.layers = layers; r.fmt = (uint32_t)fmt; r.frame = R.frame; r.caller = caller;
    auto old = R.imgLive.find(r.image);
    if (old != R.imgLive.end())   // (untracked destroy path: retire the old record)
    {
        if (old->second.lo) R.imgByVa.erase(old->second.lo);
        R.pushImgFreed(old->second);
    }
    R.imgLive[r.image] = r;
    if (va) R.imgByVa[va] = r.image;
}

void vkImgUntrack(VkImage img)
{
    if (!img) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.imgLive.find((uint64_t)(uintptr_t)img);
    if (it == R.imgLive.end()) return;
    if (it->second.lo)
    {
        auto v = R.imgByVa.find(it->second.lo);
        if (v != R.imgByVa.end() && v->second == it->first) R.imgByVa.erase(v);
    }
    R.pushImgFreed(it->second);
    R.imgLive.erase(it);
}

void vkImgName(VkImage img, const char* name)
{
    if (!img || !name) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    auto it = R.imgLive.find((uint64_t)(uintptr_t)img);
    if (it != R.imgLive.end()) std::snprintf(it->second.name, sizeof(it->second.name), "%s", name);
}

void vkViewTrack(VkImageView v, VkImage img)
{
    if (!v) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    AddrRegistry::ViewRec rec;
    rec.serial = R.nextViewSerial++;
    rec.image = (uint64_t)(uintptr_t)img;
    auto im = R.imgLive.find(rec.image);
    rec.imgSerial = im != R.imgLive.end() ? im->second.serial : 0;
    R.views[(uint64_t)(uintptr_t)v] = rec;
}

void vkViewUntrack(VkImageView v)
{
    if (!v) return;
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    R.views.erase((uint64_t)(uintptr_t)v);
}

// ---- descriptor tracking ----
namespace
{
std::atomic<bool> sDescTrack{false};

bool isImageDescriptor(VkDescriptorType t)
{
    return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
        || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}

void descSetEntry(AddrRegistry& R, uint64_t set, uint32_t binding, const DescEntry* e)
{
    auto& v = R.descSets[set];
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].binding == binding)
        {
            if (e) v[i] = *e; else { v[i] = v.back(); v.pop_back(); }
            return;
        }
    if (e) v.push_back(*e);
}

void descStaleLine(const char* line)
{
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
    if (gVkFaultLogSink) gVkFaultLogSink(line);
}
} // namespace

void vkDescTrackEnable(bool on)
{
    if (sDescTrack.load(std::memory_order_relaxed) == on) return;
    AddrRegistry& R = addrReg();
    {
        std::lock_guard<std::mutex> lk(R.m);
        R.descSets.clear();   // records from before were not kept up to date
        R.descReported.clear();
    }
    sDescTrack.store(on, std::memory_order_relaxed);
    char line[160];
    std::snprintf(line, sizeof(line), "[vktest] descriptor tracking %s (stale-descriptor check at bind time)",
                  on ? "ON" : "off");
    descStaleLine(line);
}

bool vkDescTrackEnabled() { return sDescTrack.load(std::memory_order_relaxed); }

uint32_t vkDescTrackStaleCount()
{
    AddrRegistry& R = addrReg();
    std::unique_lock<std::mutex> lk(R.m, std::try_to_lock);   // (also called on the crash path)
    return lk.owns_lock() ? R.descStale : 0u;
}

// ---- copy-on-write set tracking (vk_common.h, vkCowTrack) ----
namespace
{
// Written elements per binding: bit e = array element e (0..kCowMaxElems-1) of that binding.
// OVERFLOW (2026-10-22, #926): a write past what the record can hold (binding >= 64, element
// >= kCowMaxElems) marks the set UNCOPYABLE -- vkCowTracked() then says no, so the re-point
// takes the device-wait path -- instead of silently leaving those elements out of the copy.
// Before this the record held elements 0..7 only, and draw set 0 binding 49 has 10 (#901: the
// avatar shelter atlas pair at 8 / 9): every copy-on-write re-point of draw set 0 left 49[8] /
// 49[9] holding whatever the recycled spare set last held -- a destroyed atlas image -- and the
// weather binding cache (mWeatherBoundViews) never rewrote them (Beta 4/5 DEVICE_LOSTs with a
// rigged re-size 1-3 frames before the loss; #926: a read inside a freed 1024 x 516 RG32F
// image = the Beta 4 shelter atlas).
constexpr uint32_t kCowMaxElems = kVkCowMaxArrayElems;
static_assert(kCowMaxElems <= 32, "CowMask::el is 32 bits per binding");
struct CowMask { uint32_t el[64] = {}; bool over = false; };
std::atomic<uint32_t>                  sCowTracked{0};   // tracked sets (0: one relaxed load per update)
std::mutex                             sCowM;
std::unordered_map<uint64_t, CowMask>  sCowMask;
std::atomic<uint32_t>                  sCowMisses{0};
std::atomic<uint64_t>                  sCowMissSet{0};
std::atomic<uint32_t>                  sCowMissBinding{0};
thread_local bool                      tCowGuard = false;
thread_local const VkDescriptorSet*    tCowFresh = nullptr;
thread_local uint32_t                  tCowFreshN = 0;

void cowMark(CowMask& m, uint32_t binding, uint32_t first, uint32_t count)
{
    // (a count running past the binding would continue into the next ones: none of the
    // tracked sets is written that way -- every write names its binding)
    if (binding >= 64u || first + count > kCowMaxElems) { m.over = true; return; }
    for (uint32_t e = first; e < first + count; ++e) m.el[binding] |= 1u << e;
}

void cowGuardOne(VkDescriptorSet dst, uint32_t binding)
{
    if (!dst) return;
    for (uint32_t i = 0; i < tCowFreshN; ++i)
        if (tCowFresh[i] == dst) return;
    if (sCowMisses.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        sCowMissSet.store((uint64_t)(uintptr_t)dst, std::memory_order_relaxed);
        sCowMissBinding.store(binding, std::memory_order_relaxed);
    }
}

void cowUpdate(uint32_t writeCount, const VkWriteDescriptorSet* writes, uint32_t copyCount,
               const VkCopyDescriptorSet* copies)
{
    if (tCowGuard)
    {
        for (uint32_t i = 0; i < writeCount; ++i) cowGuardOne(writes[i].dstSet, writes[i].dstBinding);
        for (uint32_t i = 0; i < copyCount; ++i) cowGuardOne(copies[i].dstSet, copies[i].dstBinding);
    }
    if (!sCowTracked.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(sCowM);
    for (uint32_t i = 0; i < writeCount; ++i)
    {
        const VkWriteDescriptorSet& w = writes[i];
        auto it = sCowMask.find((uint64_t)(uintptr_t)w.dstSet);
        if (it != sCowMask.end()) cowMark(it->second, w.dstBinding, w.dstArrayElement, w.descriptorCount);
    }
    for (uint32_t i = 0; i < copyCount; ++i)
    {
        const VkCopyDescriptorSet& c = copies[i];
        auto it = sCowMask.find((uint64_t)(uintptr_t)c.dstSet);
        if (it != sCowMask.end()) cowMark(it->second, c.dstBinding, c.dstArrayElement, c.descriptorCount);
    }
}
} // namespace

void vkCowTrack(VkDescriptorSet set, bool on)
{
    if (!set) return;
    std::lock_guard<std::mutex> lk(sCowM);
    if (on) sCowMask[(uint64_t)(uintptr_t)set] = CowMask{};   // (re-tracking starts empty)
    else sCowMask.erase((uint64_t)(uintptr_t)set);
    sCowTracked.store((uint32_t)sCowMask.size(), std::memory_order_relaxed);
}

bool vkCowTracked(VkDescriptorSet set)
{
    std::lock_guard<std::mutex> lk(sCowM);
    auto it = sCowMask.find((uint64_t)(uintptr_t)set);
    return it != sCowMask.end() && !it->second.over;   // (an overflowed record cannot be copied)
}

bool vkCowCopySet(VkDevice dev, VkDescriptorSet src, VkDescriptorSet dst)
{
    if (!dev || !src || !dst || src == dst) return false;
    std::vector<VkCopyDescriptorSet> cps;
    {
        std::lock_guard<std::mutex> lk(sCowM);
        auto it = sCowMask.find((uint64_t)(uintptr_t)src);
        if (it == sCowMask.end() || it->second.over) return false;
        const CowMask m = it->second;
        sCowMask[(uint64_t)(uintptr_t)dst] = m;   // the copy holds exactly what the source held
        sCowTracked.store((uint32_t)sCowMask.size(), std::memory_order_relaxed);
        for (uint32_t b = 0; b < 64u; ++b)
            for (uint32_t e = 0; e < kCowMaxElems; ++e)
                if (m.el[b] & (1u << e))
                {
                    VkCopyDescriptorSet c{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
                    c.srcSet = src; c.srcBinding = b; c.srcArrayElement = e;
                    c.dstSet = dst; c.dstBinding = b; c.dstArrayElement = e;
                    c.descriptorCount = 1;
                    cps.push_back(c);
                }
    }
    // A pending set is a valid copy SOURCE (only the destination must not be in use).
    if (!cps.empty()) vkUpdateDescriptorSets(dev, 0, nullptr, (uint32_t)cps.size(), cps.data());
    return true;
}

void vkCowGuardBegin(const VkDescriptorSet* fresh, uint32_t n)
{
    tCowFresh = fresh;
    tCowFreshN = fresh ? n : 0u;
    tCowGuard = true;
}

void vkCowGuardEnd()
{
    tCowGuard = false;
    tCowFresh = nullptr;
    tCowFreshN = 0;
}

uint32_t vkCowGuardMisses(uint64_t* first_set, uint32_t* first_binding)
{
    if (first_set) *first_set = sCowMissSet.load(std::memory_order_relaxed);
    if (first_binding) *first_binding = sCowMissBinding.load(std::memory_order_relaxed);
    return sCowMisses.load(std::memory_order_relaxed);
}

// Records the writes / copies (caller holds nothing; called before the real update).
static void descTrackUpdate(uint32_t writeCount, const VkWriteDescriptorSet* writes,
                            uint32_t copyCount, const VkCopyDescriptorSet* copies)
{
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    for (uint32_t i = 0; i < writeCount; ++i)
    {
        const VkWriteDescriptorSet& w = writes[i];
        if (!w.dstSet || w.dstArrayElement != 0) continue;   // bindless arrays: not tracked
        const uint64_t set = (uint64_t)(uintptr_t)w.dstSet;
        if (w.descriptorCount == 1 && isImageDescriptor(w.descriptorType) && w.pImageInfo && w.pImageInfo[0].imageView)
        {
            DescEntry e;
            e.binding = w.dstBinding;
            e.view = (uint64_t)(uintptr_t)w.pImageInfo[0].imageView;
            auto vr = R.views.find(e.view);
            if (vr == R.views.end()) { descSetEntry(R, set, w.dstBinding, nullptr); continue; }   // unknown view
            e.viewSerial = vr->second.serial;
            e.imgSerial = vr->second.imgSerial;
            descSetEntry(R, set, w.dstBinding, &e);
        }
        else
            descSetEntry(R, set, w.dstBinding, nullptr);   // not a single image descriptor: forget
    }
    for (uint32_t i = 0; i < copyCount; ++i)
    {
        const VkCopyDescriptorSet& c = copies[i];
        if (!c.dstSet || c.dstArrayElement != 0) continue;
        const uint64_t dst = (uint64_t)(uintptr_t)c.dstSet;
        const DescEntry* src = nullptr;
        if (c.descriptorCount == 1 && c.srcArrayElement == 0)
        {
            auto s = R.descSets.find((uint64_t)(uintptr_t)c.srcSet);
            if (s != R.descSets.end())
                for (const DescEntry& e : s->second) if (e.binding == c.srcBinding) { src = &e; break; }
        }
        if (src) { DescEntry e = *src; e.binding = c.dstBinding; descSetEntry(R, dst, c.dstBinding, &e); }
        else descSetEntry(R, dst, c.dstBinding, nullptr);
    }
}

static void descTrackBind(uint32_t setCount, const VkDescriptorSet* sets)
{
    AddrRegistry& R = addrReg();
    std::lock_guard<std::mutex> lk(R.m);
    for (uint32_t s = 0; s < setCount; ++s)
    {
        if (!sets[s]) continue;
        const uint64_t set = (uint64_t)(uintptr_t)sets[s];
        auto it = R.descSets.find(set);
        if (it == R.descSets.end()) continue;
        for (const DescEntry& e : it->second)
        {
            auto vr = R.views.find(e.view);
            if (vr != R.views.end() && vr->second.serial == e.viewSerial) continue;   // still the same view
            const uint64_t key = set * 1000003ull ^ ((uint64_t)e.binding << 48) ^ e.viewSerial;
            if (!R.descReported.insert(key).second) continue;   // logged already
            ++R.descStale;
            const ImgRange* was = R.findImgBySerial(e.imgSerial);
            const ImgRange* now = nullptr;
            if (vr != R.views.end())
            {
                auto im = R.imgLive.find(vr->second.image);
                if (im != R.imgLive.end()) now = &im->second;
            }
            char line[512];
            std::snprintf(line, sizeof(line),
                "[vktest] STALE DESCRIPTOR bound: set 0x%llx binding %u names view 0x%llx of image '%s' %ux%u "
                "(created frame %u%s%s) -- %s%s%s (frame %u). A set naming a re-created target must follow "
                "the render-target generation (llcleanrenderervk.h, mTargetGen).",
                (unsigned long long)set, e.binding, (unsigned long long)e.view,
                was && was->name[0] ? was->name : "-", was ? was->w : 0u, was ? was->h : 0u,
                was ? was->frame : 0u, was && was->freedFrame ? ", FREED frame " : "",
                was && was->freedFrame ? std::to_string(was->freedFrame).c_str() : "",
                vr == R.views.end() ? "the view was destroyed" : "the view handle was RECYCLED for image '",
                vr == R.views.end() ? "" : (now && now->name[0] ? now->name : "-"),
                vr == R.views.end() ? "" : "'", R.frame);
            descStaleLine(line);
        }
    }
}

// RenderVKPipelineStats: the device-wide context for the per-pipeline statistics -- the
// subgroup size(s) and, where the vendor extension is listed, the register file / SIMD
// shape (AMD shader core properties) or SM / warp counts (NV shader SM builtins).
std::string VkDeviceContext::shaderCoreSummary() const
{
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ep(en);
    vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
    auto listed = [&](const char* n) {
        for (const auto& e : ep) if (std::strcmp(e.extensionName, n) == 0) return true;
        return false;
    };
    VkPhysicalDeviceProperties base{};
    vkGetPhysicalDeviceProperties(mPhysical, &base);
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    p2.pNext = &sg;
    VkPhysicalDeviceSubgroupSizeControlPropertiesEXT sgc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES_EXT};
    const bool haveSgc = base.apiVersion >= VK_API_VERSION_1_3 || listed(VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    if (haveSgc) { sgc.pNext = p2.pNext; p2.pNext = &sgc; }
    VkPhysicalDeviceShaderCorePropertiesAMD amd{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD};
    const bool haveAmd = listed(VK_AMD_SHADER_CORE_PROPERTIES_EXTENSION_NAME);
    if (haveAmd) { amd.pNext = p2.pNext; p2.pNext = &amd; }
    VkPhysicalDeviceShaderSMBuiltinsPropertiesNV nv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV};
    const bool haveNv = listed(VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME);
    if (haveNv) { nv.pNext = p2.pNext; p2.pNext = &nv; }
    vkGetPhysicalDeviceProperties2(mPhysical, &p2);
    std::ostringstream o;
    o << "subgroup size " << sg.subgroupSize;
    if (haveSgc) o << " (min " << sgc.minSubgroupSize << ", max " << sgc.maxSubgroupSize << ")";
    o << "; max compute shared memory " << base.limits.maxComputeSharedMemorySize << " B, max workgroup invocations "
      << base.limits.maxComputeWorkGroupInvocations;
    if (haveAmd)
        o << "; AMD shader core: wavefront " << amd.wavefrontSize << ", " << amd.wavefrontsPerSimd << " wavefronts/SIMD, "
          << amd.simdPerComputeUnit << " SIMD/CU, "
          << amd.shaderEngineCount * amd.shaderArraysPerEngineCount * amd.computeUnitsPerShaderArray << " CUs; VGPRs/SIMD "
          << amd.vgprsPerSimd << " (alloc " << amd.minVgprAllocation << "-" << amd.maxVgprAllocation << ", granularity "
          << amd.vgprAllocationGranularity << "), SGPRs/SIMD " << amd.sgprsPerSimd << " (alloc " << amd.minSgprAllocation
          << "-" << amd.maxSgprAllocation << ", granularity " << amd.sgprAllocationGranularity << ")";
    if (haveNv)
        o << "; NVIDIA " << nv.shaderSMCount << " SMs, " << nv.shaderWarpsPerSM
          << " warps/SM (register file: 64K 32-bit registers per SM, at most 255 per thread)";
    return o.str();
}

void VkDeviceContext::setObjectName(VkObjectType type, uint64_t handle, const char* name) const
{
    // The image registry keeps the name for DEVICE_LOST reports / stale-descriptor logs
    // (always, not only with validation).
    if (type == VK_OBJECT_TYPE_IMAGE && handle && name) vkImgName((VkImage)(uintptr_t)handle, name);
    // (#973: also with debug labels on -- the instance then has VK_EXT_debug_utils too)
    if (!(mValidation || sDebugUtilsOn) || !mDevice || !handle || !name) return;
    static PFN_vkSetDebugUtilsObjectNameEXT fn = nullptr;
    if (!fn) fn = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(mInstance, "vkSetDebugUtilsObjectNameEXT");
    if (!fn) return;
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type; ni.objectHandle = handle; ni.pObjectName = name;
    fn(mDevice, &ni);
}

// #874: the data fault addresses of the last report (the RT scene audit checks them).
static VkDeviceAddress sFaultAddrs[16] = {};
static uint32_t sFaultAddrN = 0;
void (*gVkRtLostAuditHook)(const VkDeviceAddress* faults, uint32_t n) = nullptr;
void (*gVkAsyncLostHook)() = nullptr;
void (*gVkRtLostAuditMainLoop)() = nullptr;
uint32_t vkLastFaultAddresses(VkDeviceAddress* out, uint32_t max)
{
    const uint32_t n = std::min(sFaultAddrN, max);
    for (uint32_t i = 0; i < n && out; ++i) out[i] = sFaultAddrs[i];
    return n;
}
void vkFaultLogLine(const char* line)
{
    vkFaultLog("%s\n", line ? line : "");
}

static void vkReportDeviceLostImpl();
static void vkReportSubmitLog();   // #845 / #917 (defined with vkSubmitLogNote, below)
static void vkReportAsBuildInputs();   // #845 / #917 (defined with vkSubmitLogNote, below)
static void VKAPI_PTR vkBuildAsNoted(VkCommandBuffer cmd, uint32_t n,
                                     const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                     const VkAccelerationStructureBuildRangeInfoKHR* const* ranges);
static PFN_vkCmdBuildAccelerationStructuresKHR sRealBuildAs = nullptr;
void vkReportDeviceLost()
{
    static std::atomic<int> s_reports{0};
    if (s_reports.fetch_add(1) >= 2) return;   // a crash path that keeps failing: report twice at most
    vkFaultLog("[vktest] DEVICE_LOST report (VK_EXT_device_fault %s, breadcrumbs %s%s)\n",
               sGetFaultInfo && sFaultDevice != VK_NULL_HANDLE ? "enabled" : "NOT available",
               sGetQueueCheckpointDataNV ? "NV checkpoints" : (sCrumbBuf.mapped ? "AMD markers" : "off"),
               sGetQueueCheckpointDataNV || sCrumbBuf.mapped ? "" : " -- RenderVKGpuBreadcrumbs");
    sFaultAddrN = 0;
    vkReportDeviceLostImpl();
    vkFaultFlushLine();
    // #874 RT scene audit: every TLAS instance reference / slot address of the last frames
    // against the registry (also on simulated / unconfirmed losses: then without addresses).
    if (gVkAsyncLostHook) { gVkAsyncLostHook(); vkFaultFlushLine(); }   // #845 / #917
    vkReportAsBuildInputs();   // #845 / #917: every AS build input address of the last frames vs the registry
    vkFaultFlushLine();
    if (gVkRtLostAuditHook) gVkRtLostAuditHook(sFaultAddrs, sFaultAddrN);
    else vkFaultLog("[vktest] RT scene audit: not available (ray tracing not running)\n");
    vkFaultFlushLine();
    std::fflush(stderr);
}

static void vkReportDeviceLostImpl()
{
    vkReportBreadcrumbs();
    vkReportSubmitLog();   // #845 / #917
    listRecentImageFrees();
    if (vkDescTrackEnabled())
        vkFaultLog("[vktest] descriptor tracking: %u stale bind(s) logged before this\n", vkDescTrackStaleCount());
    if (!sLostConfirmed.load(std::memory_order_acquire))
    {
        // vkGetDeviceFaultInfoEXT requires a lost device (VUID-vkGetDeviceFaultInfoEXT-device-07336).
        vkFaultLog("[vktest] DEVICE_LOST: %s, fault query skipped\n",
                   vkDeviceLostSimulated() ? "simulated loss (RenderVKDebugSimulateDeviceLost)"
                                           : "not confirmed by the driver (GPU hang watchdog / lost surface)");
        return;
    }
    if (!sGetFaultInfo || sFaultDevice == VK_NULL_HANDLE)
    {
        vkFaultLog("[vktest] DEVICE_LOST: no VK_EXT_device_fault info available\n");
        return;
    }
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (sGetFaultInfo(sFaultDevice, &counts, nullptr) != VK_SUCCESS &&
        counts.addressInfoCount == 0 && counts.vendorInfoCount == 0)
    {
        // #973: no fault record at all -- what a hang (TDR) leaves on AMD / Windows.
        vkFaultLog("[vktest] DEVICE_LOST: vkGetDeviceFaultInfoEXT returned nothing -- no fault address: GPU hang / "
                   "timeout (TDR), not a page fault (the breadcrumbs above say which pass was running)\n");
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vend(counts.vendorInfoCount);
    counts.vendorBinarySize = 0;   // no vendor binary dump
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addrs.empty() ? nullptr : addrs.data();
    info.pVendorInfos  = vend.empty()  ? nullptr : vend.data();
    sGetFaultInfo(sFaultDevice, &counts, &info);
    vkFaultLog("[vktest] DEVICE_LOST fault: %s\n", info.description);
    // #973: a hang (TDR) reports no DATA address (an AMD RX 9070 XT hang in the RT shadow mask
    // read as a bare description); say so instead of leaving the reader to infer it from an
    // empty list -- a page fault (#874 / #889) names the address the GPU could not translate.
    {
        uint32_t dataAddrs = 0, ipAddrs = 0;
        for (const auto& a : addrs)
        {
            if (a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT) continue;
            const bool ip = a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT ||
                            a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT ||
                            a.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT;
            ++(ip ? ipAddrs : dataAddrs);
        }
        if (dataAddrs == 0)
            vkFaultLog("[vktest] DEVICE_LOST: no fault address%s: GPU hang / timeout (TDR), not a page fault "
                       "(%u vendor record(s)%s)\n",
                       ipAddrs ? " (instruction pointers only)" : "", (unsigned)vend.size(),
                       vend.empty() ? "" : ", below");
    }
    for (const auto& a : addrs)
    {
        vkFaultLog("[vktest]   address type=%d addr=0x%llx precision=%llu\n",
                     (int)a.addressType, (unsigned long long)a.reportedAddress,
                     (unsigned long long)a.addressPrecision);
        // Data addresses only (types 1-5: invalid/unmapped read/write/execute).
        if (a.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT &&
            a.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT &&
            a.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT &&
            a.addressType != VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT)
        {
            mapFaultAddress(a.reportedAddress, a.addressPrecision);
            if (sFaultAddrN < 16) sFaultAddrs[sFaultAddrN++] = a.reportedAddress;
        }
    }
    for (const auto& v : vend)
        vkFaultLog("[vktest]   vendor: %s code=0x%llx data=0x%llx\n",
                     v.description, (unsigned long long)v.vendorFaultCode,
                     (unsigned long long)v.vendorFaultData);
    if (vend.empty()) vkFaultLog("[vktest]   vendor fault info: none\n");
    std::fflush(stderr);
}

const char* vkResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        default: return "VK_ERROR_<other>";
    }
}

// ---------------------------------------------------------------------------
// Debug messenger
// ---------------------------------------------------------------------------
// VK_EXT_device_memory_report (#407): driver-side allocations, attributed by object type.
static VKAPI_ATTR void VKAPI_CALL deviceMemoryReportCb(const VkDeviceMemoryReportCallbackDataEXT* data, void* user)
{
    VkDeviceContext* ctx = static_cast<VkDeviceContext*>(user);
    if (ctx && data) ctx->onDriverMemoryReport(*data);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* user) {
    auto* ctx = reinterpret_cast<VkDeviceContext*>(user);
    if (ctx && data) ctx->onDebugMessage(severity, data->pMessage);
    return VK_FALSE;
}

void VkDeviceContext::onDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                     const char* msg) {
    const char* tag = "INFO";
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        tag = "ERROR"; ++mValidationErrors;
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        tag = "WARN"; ++mValidationWarnings;
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        tag = "VERBOSE"; // KEEP: debugPrintf + GPU-AV detail arrive at INFO/VERBOSE
    }
    // Log everything (incl. VERBOSE/INFO) -- diagnosis needs debugPrintf / GPU-AV
    // output that the layer emits below ERROR/WARNING. Env-gated (validation off by
    // default), so this only fires on SLIPSTREAM_VK_VALIDATION runs.
    std::fprintf(stderr, "[validation:%s] %s\n", tag, msg);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
bool VkDeviceContext::createInstance(bool enableValidation,
        const std::vector<const char*>& extraInstanceExts) {
    mReqReport = VkRequirementsReport{};
    {
        uint32_t loader = 0;
        if (vkEnumerateInstanceVersion(&loader) == VK_SUCCESS) mReqReport.loaderVersion = loader;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "slipstream";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "slipstream-vk";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> exts;
    if (enableValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    // #973 debug labels: VK_EXT_debug_utils without validation, where the loader lists it.
    bool debugUtils = enableValidation;
    if (gVkDebugLabelsWanted && !debugUtils) {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> props(n);
        if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data());
        for (uint32_t i = 0; i < n && !debugUtils; ++i)
            debugUtils = std::strcmp(props[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        if (debugUtils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    // Merge caller-supplied instance extensions (surface extensions from SDL),
    // de-duplicated against what we already requested.
    for (const char* e : extraInstanceExts) {
        bool dup = false;
        for (const char* have : exts) {
            if (std::strcmp(have, e) == 0) { dup = true; break; }
        }
        if (!dup) exts.push_back(e);
    }

    // Ask the validation layer to also run synchronization validation via
    // VK_EXT_layer_settings (core-adjacent, supported by the Khronos layer).
    const char* layerName = "VK_LAYER_KHRONOS_validation";
    VkBool32 valTrue = VK_TRUE;

    VkLayerSettingEXT settings[3]{};
    settings[0].pLayerName = layerName;
    settings[0].pSettingName = "validate_sync";
    settings[0].type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
    settings[0].valueCount = 1;
    settings[0].pValues = &valTrue;

    // Raise the per-VUID duplicate report cap (default 10) so the stress test
    // surfaces every DISTINCT hazard instead of masking later ones behind the
    // first VUID's dedup limit. 0 = unlimited.
    uint32_t dupLimit = 0;
    settings[1].pLayerName = layerName;
    settings[1].pSettingName = "duplicate_message_limit";
    settings[1].type = VK_LAYER_SETTING_TYPE_UINT32_EXT;
    settings[1].valueCount = 1;
    settings[1].pValues = &dupLimit;

    // CRITICAL for the #29 experiment: by default sync-validation does NOT track
    // buffer/image accesses made through descriptors *inside shaders* (it can't
    // statically know which descriptor a shader dynamically indexes). That means
    // a race between a buffer write and a shader SSBO read is INVISIBLE unless
    // this heuristic is enabled — which is precisely the #29 pattern (compute /
    // vertex shader reading an SSBO that a transfer/host write just stomped).
    // Enable it so shader-side RAW/WAR hazards are caught, not just
    // fixed-function ones (indirect-command reads, attachments, etc.).
    settings[2].pLayerName = layerName;
    settings[2].pSettingName = "syncval_shader_accesses_heuristic";
    settings[2].type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
    settings[2].valueCount = 1;
    settings[2].pValues = &valTrue;

    VkLayerSettingsCreateInfoEXT layerSettings{
        VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT};
    layerSettings.settingCount = 3;
    layerSettings.pSettings = settings;

    // FULL validation feature set for cull diagnosis (VK_EXT_validation_features).
    // GPU-assisted validation instruments shaders to catch compute SSBO
    // out-of-bounds / stale-descriptor / uninitialized-descriptor reads -- exactly
    // the suspect for "cull emits 0 draws with healthy CPU counters". Plus
    // best-practices and synchronization validation. NOTE: GPU-assisted and
    // debugPrintf are mutually exclusive in some layer versions; we PREFER
    // GPU-assisted (our priority is catching the OOB/stale read) and omit
    // debugPrintf to avoid a create-instance rejection. The per-reason atomic
    // counters already cover shader-side printf-style tracing.
    const VkValidationFeatureEnableEXT valEnables[] = {
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT,
        VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT,
        VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT valFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    valFeatures.enabledValidationFeatureCount =
        (uint32_t)(sizeof(valEnables) / sizeof(valEnables[0]));
    valFeatures.pEnabledValidationFeatures = valEnables;
    // Chain: ci.pNext -> layerSettings -> valFeatures.
    layerSettings.pNext = &valFeatures;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    if (enableValidation) ci.pNext = &layerSettings;

    VkResult vr = vkCreateInstance(&ci, nullptr, &mInstance);
    if (enableValidation && vr != VK_SUCCESS) {
        // The layer may reject the GPU-assisted feature set (old layer / no support).
        // Fall back to validation WITHOUT the extra features so we still get the
        // core layer + sync heuristic rather than aborting the diagnostic run.
        std::fprintf(stderr, "[validation:WARN] vkCreateInstance with full feature "
            "set failed (%s); retrying without VK_EXT_validation_features\n",
            vkResultString(vr));
        std::fflush(stderr);
        // Drop the validation-features extension + its pNext.
        std::vector<const char*> exts2;
        for (const char* e : exts)
            if (std::strcmp(e, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) != 0)
                exts2.push_back(e);
        layerSettings.pNext = nullptr;
        ci.enabledExtensionCount = (uint32_t)exts2.size();
        ci.ppEnabledExtensionNames = exts2.data();
        vr = vkCreateInstance(&ci, nullptr, &mInstance);
    }
    if (vr == VK_SUCCESS && vkSimulateUnsupportedMode() == "noinstance") {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
        vr = VK_ERROR_INCOMPATIBLE_DRIVER;   // SLIPSTREAM_VK_SIMULATE_UNSUPPORTED=noinstance
    }
    // #526: no Vulkan driver (VK_ERROR_INCOMPATIBLE_DRIVER) or a broken install is a
    // user-facing "can't run here", not a crash: report it to the caller.
    mReqReport.instanceResult = vr;
    if (vr != VK_SUCCESS) {
        mInstance = VK_NULL_HANDLE;
        mReqReport.failure = std::string("vkCreateInstance: ") + vkResultString(vr);
        std::fprintf(stderr, "[vkgfx] vkCreateInstance failed: %s\n", vkResultString(vr));
        std::fflush(stderr);
        return false;
    }
    mReqReport.instanceOk = true;
    // #973 debug labels (vkBreadcrumb). The loader's trampoline forwards to the driver / any
    // capture layer, and is a no-op where neither implements it.
    sDebugUtilsOn = debugUtils;
    sCmdInsertLabel = debugUtils && gVkDebugLabelsWanted
        ? (PFN_vkCmdInsertDebugUtilsLabelEXT)vkGetInstanceProcAddr(mInstance, "vkCmdInsertDebugUtilsLabelEXT")
        : nullptr;
    VK_LOG("debug labels (RenderVKDebugLabels): %s", sCmdInsertLabel ? "on (VK_EXT_debug_utils command-buffer labels)"
           : gVkDebugLabelsWanted ? "unavailable (VK_EXT_debug_utils not listed by the loader)" : "off");
    if (enableValidation) {
        std::fprintf(stderr, "[validation:INFO] validation ENABLED: features requested "
            "= GPU_ASSISTED, GPU_ASSISTED_RESERVE_BINDING_SLOT, BEST_PRACTICES, "
            "SYNCHRONIZATION_VALIDATION (+sync heuristic, unlimited dup limit)%s\n",
            (layerSettings.pNext ? "" : " [features dropped: layer rejected them]"));
        std::fflush(stderr);
    }

    if (enableValidation) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT m{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            // Widen to VERBOSE|INFO|WARNING|ERROR so debugPrintf / GPU-AV detail
            // (emitted at INFO/VERBOSE) is not filtered before the callback.
            m.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            m.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            m.pfnUserCallback = debugCb;
            m.pUserData = this;
            VK_CHECK(create(mInstance, &m, nullptr, &mMessenger));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Physical device selection (#526): the best device that meets the requirements
// (vk_requirements.h) -- discrete, then integrated, then virtual, then other; first
// enumerated wins a tie. A device that fails them is never picked, so a hybrid laptop
// with an outdated iGPU driver still runs on its capable dGPU (and vice versa).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ASYNC COMPUTE AUTO DEFAULT (RenderVKAsyncCompute -1): on or off per GPU architecture and
// driver. Table, sources and confidence per row: research/ASYNC_COMPUTE_BY_ARCH.md (local).
// Rule: off where the benefit is unproven or documented as poor AND there is device-loss
// correlation or no testing at all; on where it is measured or documented beneficial.
//   NVIDIA, NVIDIA's driver:
//     pre-Turing (Maxwell / Pascal / Volta; proxy: no pipeline fragment shading rates
//       (VK_KHR_fragment_shading_rate), which every Turing and later part has) -> OFF.
//       NVIDIA "Vulkan Dos and Don'ts": on pre-Ampere GPUs, compute on the graphics queue
//       beside compute on the async queue leaves gaps in the async queue (this renderer does
//       that); a Pascal user saw no difference with it off; a GTX 1080 Ti (Windows) lost the
//       device with graphics in c_shadows and async in q_ao.
//     Turing and later -> ON. Ampere+: NVIDIA calls compute-over-compute overlap very
//       efficient; RTX 5090 measured 14.5 vs 15.5 ms GPU at 4K (async deferred lighting).
//       Turing: the pre-Ampere caveat applies but no crash data (medium confidence). The RTX
//       3060 Windows device loss is a tracked bug (RenderVKAsyncLayoutsOnGraphics), not a
//       default.
//   NVIDIA on another driver (NVK) -> OFF: untested.
//   AMD:
//     integrated (APUs, Steam Deck Van Gogh) -> OFF: few CUs, shared memory bandwidth,
//       untested (GPU_COMPAT_845 proposed off until verified).
//     RDNA2 (Navi 21-24, 0x73A0-0x743F) on AMD's PAL drivers (proprietary, AMDVLK) -> OFF
//       (#845: device loss even with any single async workload disabled).
//     other discrete (GCN, RDNA1/3/4 on any driver, RDNA2 on RADV) -> ON: GPUOpen documents
//       the async compute queue as the overlap path on GCN and RDNA.
//   Intel (Gen9 / Gen11 / Xe-LP iGPUs, Arc Alchemist, Battlemage) -> OFF: Intel's guides only
//     say "always profile" (no documented benefit), Mesa ANV's compute engine began as an
//     opt-in (INTEL_COMPUTE_CLASS), and it was never tested here.
//   Anything else (other vendors, software rasterizers) -> OFF: untested.
// ---------------------------------------------------------------------------
bool VkDeviceContext::asyncAutoDecide(const VkPhysicalDeviceProperties& p, std::string& reason) const
{
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(mPhysical, &p2);
    char buf[96];
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        reason = "software rasterizer";
        return false;
    }
    if (p.vendorID == 0x10DE) {
        if (drv.driverID != VK_DRIVER_ID_NVIDIA_PROPRIETARY) {
            std::snprintf(buf, sizeof(buf), "NVIDIA GPU on driver id %d (not NVIDIA's driver): untested", (int)drv.driverID);
            reason = buf;
            return false;
        }
        bool fsr = false;
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) fsr = true;
        if (fsr) {
            VkPhysicalDeviceFragmentShadingRateFeaturesKHR qv{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qv;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            fsr = qv.pipelineFragmentShadingRate == VK_TRUE;
        }
        if (!fsr) {
            reason = "NVIDIA pre-Turing (Maxwell / Pascal / Volta): no concurrent graphics+compute benefit "
                     "documented before Ampere, device losses seen on Windows";
            return false;
        }
        reason = "NVIDIA Turing or newer: dedicated compute queue overlap";
        return true;
    }
    if (p.vendorID == 0x1002) {
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
            reason = "AMD integrated GPU (APU): few CUs and shared memory bandwidth, untested";
            return false;
        }
        const bool pal = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY || drv.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE;
        if (pal && p.deviceID >= 0x73A0 && p.deviceID <= 0x743F) {
            std::snprintf(buf, sizeof(buf), "#845: AMD RDNA2 (0x%04x) on AMD's own driver: device losses", p.deviceID);
            reason = buf;
            return false;
        }
        reason = drv.driverID == VK_DRIVER_ID_MESA_RADV ? "AMD discrete GPU on RADV: async compute queue overlap"
                                                         : "AMD discrete GPU: async compute queue overlap";
        return true;
    }
    if (p.vendorID == 0x8086) {
        reason = "Intel: no documented benefit, untested";
        return false;
    }
    std::snprintf(buf, sizeof(buf), "GPU vendor 0x%04x: untested", p.vendorID);
    reason = buf;
    return false;
}

bool VkDeviceContext::pickPhysicalDevice(bool wantSwapchain) {
    // The report (one line per device + the choice) is logged by the caller
    // (LLVulkanContext::init: vkRequirementsLogLines).
    vkEvaluateDevices(mInstance, wantSwapchain, mReqReport);
    if (mReqReport.instanceResult != VK_SUCCESS) {
        mReqReport.instanceOk = false;
        mReqReport.failure = std::string("vkEnumeratePhysicalDevices: ") + vkResultString(mReqReport.instanceResult);
        return false;
    }
    if (mReqReport.best < 0) {
        mReqReport.failure = mReqReport.gpus.empty() ? "no Vulkan devices"
                                                     : "no device meets the requirements";
        return false;
    }
    mPhysical = mReqReport.gpus[mReqReport.best].device;
    mApi12 = mReqReport.gpus[mReqReport.best].useApi12;   // #675

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(mPhysical, &p);
    mLimits = p.limits;
    VK_LOG("GPU: %s  (Vulkan %u.%u.%u)", p.deviceName,
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
           VK_VERSION_PATCH(p.apiVersion));
    if (mApi12)
        VK_LOG("Vulkan API path: 1.2 + extensions (VK_KHR_dynamic_rendering, VK_KHR_synchronization2, "
               "VK_EXT_extended_dynamic_state, VK_EXT_shader_demote_to_helper_invocation)%s",
               p.apiVersion >= VK_API_VERSION_1_3 ? " -- FORCED on a 1.3 device (RenderVKForceApi12)" : "");
    vkGetPhysicalDeviceMemoryProperties(mPhysical, &mMemProps);
    detectUma(p.deviceType);   // #695

    // Find a queue family with graphics (implies compute+transfer on NVIDIA).
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysical, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { mQueueFamily = i; break; }
    }

    // ASYNC COMPUTE (phase 3a, RenderVKAsyncComputeQueue). Mode 1 prefers a family with
    // COMPUTE and no GRAPHICS (NVIDIA family 2: the async-compute hardware queues) that
    // also writes timestamps and can run the RT builds; otherwise (and in mode 2) a
    // second queue of the graphics family when it exposes more than one.
    mComputeFamily = ~0u;
    mComputeQueueIndex = 0;
    mComputeOnly = false;
    mComputeTsBits = 0;
    // AUTO DEFAULT (RenderVKAsyncCompute -1 with RenderVKAsyncComputeQueue at its default; the
    // caller sets mAsyncAuto): the per-architecture decision (asyncAutoDecide below; table +
    // sources in research/ASYNC_COMPUTE_BY_ARCH.md). Off means NO second queue: the
    // pre-phase-3 device (every resource EXCLUSIVE, every async workload, the
    // RenderVKAsyncDeferredLighting auto and RenderVKFramePipelining on the graphics queue),
    // the same device the post-device-loss safe settings fall back to. An explicit
    // RenderVKAsyncCompute 0 / 1 or a user-set RenderVKAsyncComputeQueue always wins: no
    // auto decision is taken then (#845's RDNA2 exclusion is now one row of the table).
    if (mAsyncAuto && mAsyncMode != 0) {
        std::string reason;
        const bool on = asyncAutoDecide(p, reason);
        mAsyncDecision = std::string("auto -> ") + (on ? "on (" : "off (") + reason + ")";
        VK_LOG("VKAsync: %s", mAsyncDecision.c_str());
        if (!on) {
            mAsyncMode = 0;
            mAsyncStatus = "off (auto: " + reason + "; RenderVKAsyncCompute 1 forces it on, restart)";
            return true;
        }
    } else if (mAsyncMode == 0) {
        mAsyncDecision = "user-set (RenderVKAsyncComputeQueue=0: no async queue)";
    } else {
        mAsyncDecision = "user-set (RenderVKAsyncCompute / RenderVKAsyncComputeQueue chosen explicitly: no per-GPU default)";
    }
    if (mAsyncMode == 0) {
        mAsyncStatus = "off (RenderVKAsyncComputeQueue=0)";
        return true;
    }
    if (mAsyncMode == 1) {
        for (uint32_t i = 0; i < qn; ++i) {
            const VkQueueFlags f = qf[i].queueFlags;
            if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && qf[i].queueCount > 0
                && qf[i].timestampValidBits > 0) {
                mComputeFamily = i;
                mComputeOnly = true;
                mComputeTsBits = qf[i].timestampValidBits;
                break;
            }
        }
    }
    if (mComputeFamily == ~0u && qf[mQueueFamily].queueCount >= 2) {
        mComputeFamily = mQueueFamily;
        mComputeQueueIndex = 1;
        mComputeTsBits = qf[mQueueFamily].timestampValidBits;
    }
    char buf[96];
    if (mComputeFamily == ~0u)
        std::snprintf(buf, sizeof(buf), "unavailable (no compute family, graphics family has 1 queue)");
    else if (mComputeOnly)
        std::snprintf(buf, sizeof(buf), "dedicated compute family %u (%u queues)", mComputeFamily,
                      qf[mComputeFamily].queueCount);
    else
        std::snprintf(buf, sizeof(buf), "graphics family %u, queue 1", mComputeFamily);
    mAsyncStatus = buf;
    // #845 / #917: the families as the driver reports them, the mode, and whether a per-vendor
    // default applied (it returned above when it did) -- device-loss reports carry this line.
    for (uint32_t i = 0; i < qn; ++i)
    {
        const VkQueueFlags f = qf[i].queueFlags;
        VK_LOG("queue family %u: flags 0x%x (%s%s%s%s%s) count %u timestampValidBits %u granularity %ux%ux%u%s%s",
               i, (unsigned)f, (f & VK_QUEUE_GRAPHICS_BIT) ? "G" : "", (f & VK_QUEUE_COMPUTE_BIT) ? "C" : "",
               (f & VK_QUEUE_TRANSFER_BIT) ? "T" : "", (f & VK_QUEUE_SPARSE_BINDING_BIT) ? "S" : "",
               (f & 0x20u /* VIDEO_DECODE */) ? "V" : "", qf[i].queueCount, qf[i].timestampValidBits,
               qf[i].minImageTransferGranularity.width, qf[i].minImageTransferGranularity.height,
               qf[i].minImageTransferGranularity.depth, i == mQueueFamily ? " [graphics queue]" : "",
               i == mComputeFamily ? " [async compute queue]" : "");
    }
    VK_LOG("async compute queue choice: RenderVKAsyncComputeQueue=%u (%s), per-GPU default: %s, "
           "compute-only family %s",
           mAsyncMode, mAsyncAuto ? "default" : "user-set", mAsyncDecision.c_str(),
           mComputeOnly ? "yes (CONCURRENT sharing)" : "no");
    return true;
}

// ---------------------------------------------------------------------------
// CLUSTER LOD (#743): optional extensions / features. WHEN (createLogicalDevice):
//   - RenderVKClusterLOD on at startup ("full"): every one the device has, as before.
//   - otherwise, RenderVKClusterDeviceFeatures >= 1 (default): the ones that are INERT while
//     unused, so cluster LOD switched on mid-session runs those tiers without a restart and
//     the renderer with it off is exactly as before. Audited (merely enabling them changes no
//     existing pipeline, descriptor layout, memory type or entry point):
//       index_type_uint8, copy_memory_indirect (KHR / NV), memory_decompression (EXT / NV),
//       cluster_acceleration_structure (LLCleanClusterRTVK fetches entry points + limits only;
//       its per-frame work runs only with cluster LOD on), shaderInt64 +
//       shaderBufferInt64Atomics, shader_image_atomic_int64: new commands / shader
//       capabilities only, nothing implicit.
//     >= 2 also mesh_shader: the extension is inert, but the renderer then adds TASK | MESH to
//       the vertex-visible bindings of its draw set 0 (llcleanrenderervk.cpp, so the cluster
//       mesh pipelines can share the set) -- a descriptor-layout change, so opt-in.
//   - startup only ("full"), never otherwise -- they are NOT inert:
//       device_generated_commands (+ maintenance5: vkGetDeviceProcAddr returns NULL for names
//       of extensions not enabled, a device-wide behaviour change; DGC is not used yet);
//       partitioned_acceleration_structure (LLCleanRTVK makes the session's TLAS partitioned
//       when the device has it and RenderVKClusterRT >= 2).
//     Each says "needs RenderVKClusterLOD on at startup (restart)" in the tier log line.
// The feature structs live here (they must outlive vkCreateDevice) and are spliced at the
// HEAD of f2.pNext, so a refused device can be retried without them by restoring the head.
// Every newer struct is behind its header guard: older Vulkan headers (CI, the Windows
// cross build) compile the block with those capabilities simply absent.
// ---------------------------------------------------------------------------
namespace
{
struct ClusterFeatureChain
{
#ifdef VK_EXT_mesh_shader
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
#endif
#ifdef VK_EXT_index_type_uint8
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT u8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
#endif
#if defined(VK_EXT_device_generated_commands) && defined(VK_KHR_maintenance5)
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
#endif
#ifdef VK_NV_memory_decompression
    // (the EXT promoted the NV struct unchanged: same sType value, aliased type)
    VkPhysicalDeviceMemoryDecompressionFeaturesNV decomp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV};
#endif
#ifdef VK_KHR_copy_memory_indirect
    VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR copyKHR{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR};
#endif
#ifdef VK_NV_copy_memory_indirect
    VkPhysicalDeviceCopyMemoryIndirectFeaturesNV copyNV{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV};
#endif
#ifdef VK_NV_cluster_acceleration_structure
    VkPhysicalDeviceClusterAccelerationStructureFeaturesNV clas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV};
#endif
#ifdef VK_NV_partitioned_acceleration_structure
    VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV ptlas{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV};
#endif
#ifdef VK_EXT_shader_image_atomic_int64
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
#endif
};

// Query the device for every cluster capability; for each one supported, append its
// extension(s) to `exts`, splice its feature struct in front of f2.pNext with the feature
// bits set, and record it in `out`. f12 / f2: the device's Vulkan 1.2 + core feature structs
// (64-bit buffer atomics are core 1.2 bits). rtEnabled gates the acceleration-structure ones.
// full: RenderVKClusterLOD on at startup (every capability); else only the inert ones (the
// block comment above), + mesh shaders when `mesh`.
void spliceClusterFeatures(VkPhysicalDevice pd, bool api12, bool rtEnabled, bool full, bool mesh,
                           ClusterFeatureChain& ch, std::vector<const char*>& exts, VkPhysicalDeviceFeatures2& f2,
                           VkPhysicalDeviceVulkan12Features& f12, VkClusterFeatures& out)
{
    out = VkClusterFeatures{};
    out.lodAtStartup = full;
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ep(en);
    if (en) vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ep.data());
    auto listed = [&](const char* name) {
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    auto already = [&](const char* name) {
        for (const char* e : exts)
            if (std::strcmp(e, name) == 0) return true;
        return false;
    };
    auto addExt = [&](const char* name) { if (!already(name)) exts.push_back(name); };
    auto query = [&](void* s) {
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = s;
        vkGetPhysicalDeviceFeatures2(pd, &q);
        return q;
    };
    auto splice = [&](void* s) {
        reinterpret_cast<VkBaseOutStructure*>(s)->pNext = reinterpret_cast<VkBaseOutStructure*>(f2.pNext);
        f2.pNext = s;
    };
    auto note = [&](const char* what) {
        if (!out.notes.empty()) out.notes += "; ";
        out.notes += what;
    };

#ifdef VK_EXT_mesh_shader
    if (listed(VK_EXT_MESH_SHADER_EXTENSION_NAME))
    {
        if (api12) note("mesh_shader: listed, off on the Vulkan 1.2 path (needs SPIR-V 1.4)");
        else if (!full && !mesh)
            note("mesh_shader: needs RenderVKClusterLOD on at startup or RenderVKClusterDeviceFeatures 2 (restart)");
        else
        {
            VkPhysicalDeviceMeshShaderFeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
            query(&q);
            if (q.meshShader && q.taskShader)
            {
                ch.mesh.taskShader = VK_TRUE;
                ch.mesh.meshShader = VK_TRUE;
                splice(&ch.mesh);
                addExt(VK_EXT_MESH_SHADER_EXTENSION_NAME);
                out.meshShader = true;
                VkPhysicalDeviceMeshShaderPropertiesEXT mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &mp;
                vkGetPhysicalDeviceProperties2(pd, &p2);
                out.meshMaxOutputVertices = mp.maxMeshOutputVertices;
                out.meshMaxOutputPrimitives = mp.maxMeshOutputPrimitives;
                out.meshMaxWorkGroupInvocations = mp.maxMeshWorkGroupInvocations;
                out.meshMaxOutputMemory = mp.maxMeshOutputMemorySize;
                out.meshMaxSharedMemory = mp.maxMeshSharedMemorySize;
                out.taskMaxPayload = mp.maxTaskPayloadSize;
            }
            else note("mesh_shader: listed, meshShader / taskShader feature missing");
        }
    }
#endif
#ifdef VK_EXT_index_type_uint8
    {   // uint8 indices: the KHR (promoted) or EXT extension; one struct, same sType.
        const char* u8ext = nullptr;
#ifdef VK_KHR_index_type_uint8
        if (listed(VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME)) u8ext = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
#endif
        if (!u8ext && listed(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME)) u8ext = VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME;
        if (u8ext)
        {
            VkPhysicalDeviceIndexTypeUint8FeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
            query(&q);
            if (q.indexTypeUint8)
            {
                ch.u8.indexTypeUint8 = VK_TRUE;
                splice(&ch.u8);
                addExt(u8ext);
                out.indexTypeUint8 = true;
            }
        }
    }
#endif
#if defined(VK_EXT_device_generated_commands) && defined(VK_KHR_maintenance5)
    if (listed(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME))
    {
        if (!full) note("device_generated_commands: needs RenderVKClusterLOD on at startup (restart)");
        else if (!listed(VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) note("device_generated_commands: needs VK_KHR_maintenance5");
        else
        {
            VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT};
            VkPhysicalDeviceMaintenance5FeaturesKHR q5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
            q.pNext = &q5;
            query(&q);
            if (q.deviceGeneratedCommands && q5.maintenance5)
            {
                ch.dgc.deviceGeneratedCommands = VK_TRUE;
                ch.dgc.dynamicGeneratedPipelineLayout = q.dynamicGeneratedPipelineLayout;
                ch.m5.maintenance5 = VK_TRUE;
                splice(&ch.dgc);
                splice(&ch.m5);
                addExt(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
                addExt(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
                out.dgc = true;
            }
        }
    }
#endif
    out.dgcNVCompute = listed("VK_NV_device_generated_commands_compute");
#ifdef VK_NV_memory_decompression
    {
        const bool ext = listed("VK_EXT_memory_decompression"), nv = listed(VK_NV_MEMORY_DECOMPRESSION_EXTENSION_NAME);
        if (ext || nv)
        {
            VkPhysicalDeviceMemoryDecompressionFeaturesNV q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_NV};
            query(&q);
            if (q.memoryDecompression)
            {
                ch.decomp.memoryDecompression = VK_TRUE;
                splice(&ch.decomp);
                addExt(ext ? "VK_EXT_memory_decompression" : VK_NV_MEMORY_DECOMPRESSION_EXTENSION_NAME);
                out.memDecompEXT = ext;
                out.memDecompNV = !ext;
                VkPhysicalDeviceMemoryDecompressionPropertiesNV dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_NV};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &dp;
                vkGetPhysicalDeviceProperties2(pd, &p2);
                out.decompressionMethods = (uint64_t)dp.decompressionMethods;
            }
        }
    }
#endif
    {   // indirect copies: the KHR, else the NV.
        bool done = false;
#ifdef VK_KHR_copy_memory_indirect
        if (listed(VK_KHR_COPY_MEMORY_INDIRECT_EXTENSION_NAME))
        {
            VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR};
            query(&q);
            if (q.indirectMemoryCopy)
            {
                ch.copyKHR.indirectMemoryCopy = VK_TRUE;
                splice(&ch.copyKHR);
                addExt(VK_KHR_COPY_MEMORY_INDIRECT_EXTENSION_NAME);
                out.copyIndirectKHR = done = true;
            }
        }
#endif
#ifdef VK_NV_copy_memory_indirect
        if (!done && listed(VK_NV_COPY_MEMORY_INDIRECT_EXTENSION_NAME))
        {
            VkPhysicalDeviceCopyMemoryIndirectFeaturesNV q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV};
            query(&q);
            if (q.indirectCopy)
            {
                ch.copyNV.indirectCopy = VK_TRUE;
                splice(&ch.copyNV);
                addExt(VK_NV_COPY_MEMORY_INDIRECT_EXTENSION_NAME);
                out.copyIndirectNV = done = true;
            }
        }
#endif
        (void)done;
    }
#ifdef VK_NV_cluster_acceleration_structure
    if (listed(VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME))
    {
        if (!rtEnabled) note("cluster_acceleration_structure: listed, ray tracing off");
        else
        {
            VkPhysicalDeviceClusterAccelerationStructureFeaturesNV q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV};
            query(&q);
            if (q.clusterAccelerationStructure)
            {
                ch.clas.clusterAccelerationStructure = VK_TRUE;
                splice(&ch.clas);
                addExt(VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                out.clusterAS = true;
                VkPhysicalDeviceClusterAccelerationStructurePropertiesNV cp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV};
                VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                p2.pNext = &cp;
                vkGetPhysicalDeviceProperties2(pd, &p2);
                out.clasMaxVerticesPerCluster = cp.maxVerticesPerCluster;
                out.clasMaxTrianglesPerCluster = cp.maxTrianglesPerCluster;
            }
        }
    }
#endif
#ifdef VK_NV_partitioned_acceleration_structure
    if (listed(VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME) && rtEnabled && !full)
        note("partitioned TLAS: needs RenderVKClusterLOD on at startup (restart)");
    else if (listed(VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME) && rtEnabled)
    {
        VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV};
        query(&q);
        if (q.partitionedAccelerationStructure)
        {
            ch.ptlas.partitionedAccelerationStructure = VK_TRUE;
            splice(&ch.ptlas);
            addExt(VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            out.partitionedAS = true;
        }
    }
#endif
    {   // 64-bit buffer atomics (core 1.2 + shaderInt64): bits on the device's own structs.
        VkPhysicalDeviceVulkan12Features q12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        const VkPhysicalDeviceFeatures2 q = query(&q12);
        if (q.features.shaderInt64 && q12.shaderBufferInt64Atomics)
        {
            f2.features.shaderInt64 = VK_TRUE;
            f12.shaderBufferInt64Atomics = VK_TRUE;
            out.bufferInt64Atomics = true;
        }
    }
#ifdef VK_EXT_shader_image_atomic_int64
    if (out.bufferInt64Atomics && listed(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME))
    {
        VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
        query(&q);
        if (q.shaderImageInt64Atomics)
        {
            ch.img64.shaderImageInt64Atomics = VK_TRUE;
            splice(&ch.img64);
            addExt(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
            out.imageInt64Atomics = true;
        }
    }
#endif
}
} // namespace

// ---------------------------------------------------------------------------
// Logical device — enable the modern feature set the port would rely on.
// ---------------------------------------------------------------------------
bool VkDeviceContext::createLogicalDevice(bool wantSwapchain) {
    // Graphics queue (+ the async compute queue: a second create info for a dedicated
    // family, or queue index 1 of the graphics family). Equal priorities: the async work
    // is on the frame's critical path too (the lit pass waits for the RT mask).
    const float prio[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qcis[3]{};
    uint32_t nqci = 1;
    VkDeviceQueueCreateInfo& qci = qcis[0];
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = mQueueFamily;
    qci.queueCount = (mComputeFamily == mQueueFamily) ? 2 : 1;
    qci.pQueuePriorities = prio;
    if (mComputeFamily != ~0u && mComputeFamily != mQueueFamily) {
        qcis[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[1].queueFamilyIndex = mComputeFamily;
        qcis[1].queueCount = 1;
        qcis[1].pQueuePriorities = prio;
        nqci = 2;
    }
    // <GMDX> INSTANT REPLAY: a queue of the video encode family (gmdxreplayvk.cpp), when the device
    // encodes H.264 in a family of its own. Dropped again if the device refuses it.
    mEncodeFamily = ~0u;
    mEncodeQueue = VK_NULL_HANDLE;
    uint32_t encFam = ~0u;
    const bool wantEncode = GMDXReplayEncoder::deviceSupport(mPhysical, encFam)
                         && encFam != mQueueFamily && encFam != mComputeFamily;
    if (wantEncode)
    {
        qcis[nqci].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[nqci].queueFamilyIndex = encFam;
        qcis[nqci].queueCount = 1;
        qcis[nqci].pQueuePriorities = prio;
        ++nqci;
    }
    // </GMDX>

    // Chain of feature structs. We enable the pieces a GPU-driven path needs so
    // the RHI foundation is proven capable, even though stages 1-4 use a subset.
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    // #675: the Vulkan 1.2 path does not chain f13 (a 1.2 device does not know the struct);
    // these extension feature structs stand in for it (chained below).
    VkPhysicalDeviceDynamicRenderingFeaturesKHR x12Dyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR x12Sync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT x12Eds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT x12Dem{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    VkPhysicalDeviceMaintenance4FeaturesKHR x12M4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR};

    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.drawIndirectCount = VK_TRUE; // for multi-draw-indirect-count later
    // Quantized geometry (#573): the world VS finds its slot's decode header at
    // gl_BaseVertex (SPIR-V DrawParameters). Core 1.1; every desktop driver has it.
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.shaderDrawParameters = VK_TRUE;
    mMaint4 = false;
    std::vector<const char*> devExtsApi12;   // #675: extra extensions for the 1.2 path (merged into devExts below)
    if (!mApi12) {
        f11.pNext = &f13;
    } else {
        x12Dyn.dynamicRendering = VK_TRUE;
        x12Sync.synchronization2 = VK_TRUE;
        x12Eds.extendedDynamicState = VK_TRUE;
        x12Dem.shaderDemoteToHelperInvocation = VK_TRUE;
        f11.pNext = &x12Dyn;
        x12Dyn.pNext = &x12Sync;
        x12Sync.pNext = &x12Eds;
        x12Eds.pNext = &x12Dem;
        // VK_KHR_maintenance4 (optional): the image memory requirement queries use it when
        // present, else vk_compat.h emulates them.
        uint32_t en4 = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en4, nullptr);
        std::vector<VkExtensionProperties> ep4(en4);
        if (en4) vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en4, ep4.data());
        for (const auto& e : ep4)
            if (std::strcmp(e.extensionName, VK_KHR_MAINTENANCE_4_EXTENSION_NAME) == 0) mMaint4 = true;
        if (mMaint4) {
            VkPhysicalDeviceMaintenance4FeaturesKHR q4{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &q4;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            mMaint4 = q4.maintenance4 == VK_TRUE;
        }
        if (mMaint4) {
            x12M4.maintenance4 = VK_TRUE;
            x12Dem.pNext = &x12M4;
            devExtsApi12.push_back(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
        }
    }
    f12.pNext = &f11;

    VkPhysicalDeviceFeatures2 f2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.multiDrawIndirect = VK_TRUE;
    // WORLD-RENDER UNLOCK: the GPU cull writes VkDrawIndexedIndirectCommand.
    // firstInstance = <instance id> (nonzero) so the vertex shader can index the
    // instance SSBO via gl_InstanceIndex. Without this feature the driver SILENTLY
    // DROPS every indirect draw whose firstInstance != 0 -- only instance id 0
    // rasterized, which is exactly the "1-2 prims visible" symptom. Core
    // VkPhysicalDeviceFeatures; supported on NVIDIA/AMD/Intel desktop.
    f2.features.drawIndirectFirstInstance = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.imageCubeArray = VK_TRUE;      // reflection-probe cube map arrays
    {   // Shadow cascades clamp casters behind the light-space near plane (GL_DEPTH_CLAMP).
        VkPhysicalDeviceFeatures sup{};
        vkGetPhysicalDeviceFeatures(mPhysical, &sup);
        // Optional features: enabling an unsupported one fails vkCreateDevice with
        // VK_ERROR_FEATURE_NOT_PRESENT, so only request what the device reports.
        f2.features.fillModeNonSolid = sup.fillModeNonSolid;   // selection wireframe (polygonMode LINE)
        // #565 BC6H probes: the encoder picks its single-layer block-texel view from an
        // array by the GPU-chosen slot (dynamically uniform per workgroup).
        mStorageImgArrayDynIdx = sup.shaderStorageImageArrayDynamicIndexing == VK_TRUE;
        f2.features.shaderStorageImageArrayDynamicIndexing = sup.shaderStorageImageArrayDynamicIndexing;
        mWideLines = sup.wideLines == VK_TRUE;                  // real setLineWidth() (LLRenderVK UI lines)
        f2.features.wideLines = sup.wideLines;
        {
            VkPhysicalDeviceProperties lp{};
            vkGetPhysicalDeviceProperties(mPhysical, &lp);
            mLineWidthMax = mWideLines ? lp.limits.lineWidthRange[1] : 1.0f;
        }
        mDepthClamp = sup.depthClamp == VK_TRUE;
        f2.features.depthClamp = mDepthClamp ? VK_TRUE : VK_FALSE;
        // Multi-attachment pipelines (glow / WBOIT-style MRT) use differing
        // per-attachment blend states (VUID-VkPipelineColorBlendStateCreateInfo-
        // pAttachments-00605). Near-universal on desktop.
        f2.features.independentBlend = sup.independentBlend;
        // Storage writes from fragment shaders: the GPU cursor pick's fragment list
        // (clean_pick.frag, the only pick, #704) and the world lit FS's overdraw counter.
        // REQUIRED (vk_requirements.cpp): a device without it never gets here. The
        // FS-invocation pipeline-statistics query cross-checks that counter (optional).
        f2.features.fragmentStoresAndAtomics = VK_TRUE;
        mFragStores = true;
        f2.features.pipelineStatisticsQuery  = sup.pipelineStatisticsQuery;
        // R8 / R16F / RG16F storage images (FSR 3.1's internal targets, the VRS rate
        // image). Universal on desktop; FSR falls back to the TAA upscaler without it.
        mStorageExtFormats = sup.shaderStorageImageExtendedFormats == VK_TRUE;
        f2.features.shaderStorageImageExtendedFormats = sup.shaderStorageImageExtendedFormats;
        // BC1-BC7 sampled textures (RenderVKTextureCompression, #263). Universal on desktop;
        // every format the encoder writes must also report SAMPLED_IMAGE + TRANSFER_DST /
        // SRC (downgrade copies) for optimal tiling, else compression stays off.
        mTexCompBC = false;
        if (sup.textureCompressionBC == VK_TRUE)
        {
            const VkFormat need[] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK,
                                     VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
                                     VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK,
                                     VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK};
            const VkFormatFeatureFlags want = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
                | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            bool all = true;
            for (VkFormat f : need)
            {
                VkFormatProperties fp{};
                vkGetPhysicalDeviceFormatProperties(mPhysical, f, &fp);
                if ((fp.optimalTilingFeatures & want) != want) all = false;
            }
            mTexCompBC = all;
        }
        f2.features.textureCompressionBC = mTexCompBC ? VK_TRUE : VK_FALSE;
    }
    if (!mApi12) {   // glslang emits OpDemoteToHelperInvocation for `discard` under SPIR-V 1.6 /
        // Vulkan 1.3 targets (VUID-VkShaderModuleCreateInfo-pCode-08740): enable
        // the core 1.3 feature when the device supports it. (The 1.2 path enables the
        // VK_EXT_shader_demote_to_helper_invocation feature above; it is a requirement there.)
        VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &q13;
        vkGetPhysicalDeviceFeatures2(mPhysical, &q);
        f13.shaderDemoteToHelperInvocation = q13.shaderDemoteToHelperInvocation;
        // Rigged-cull subgroup scan (clean_wg_scan.glsl): its cross-subgroup prefix needs
        // FULL compute subgroups (VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT),
        // plus basic + arithmetic subgroup ops in the compute stage and a max subgroup
        // size that divides its 256-invocation workgroup. Anything less: the shared-memory
        // scan (wgScanFullSubgroups() false).
        f13.subgroupSizeControl = q13.subgroupSizeControl;
        f13.computeFullSubgroups = q13.computeFullSubgroups;
        VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        sg.pNext = &p13;
        VkPhysicalDeviceProperties2 pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        pp.pNext = &sg;
        vkGetPhysicalDeviceProperties2(mPhysical, &pp);
        const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        const uint32_t maxSg = std::max(p13.maxSubgroupSize, sg.subgroupSize);
        mWgScanFullSubgroups = q13.subgroupSizeControl == VK_TRUE && q13.computeFullSubgroups == VK_TRUE
                            && (sg.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0
                            && (sg.supportedOperations & ops) == ops
                            && maxSg >= 4u && maxSg <= 256u && (256u % maxSg) == 0u;
        mSubgroupSize = sg.subgroupSize;
        mSubgroupSizeMin = p13.minSubgroupSize;
        mSubgroupSizeMax = p13.maxSubgroupSize;
        mSubgroupOps = (uint32_t)sg.supportedOperations;
        mComputeFullSubgroups = q13.computeFullSubgroups == VK_TRUE;
    }
    f2.pNext = &f12;

    // Ray tracing (VK_KHR_acceleration_structure + VK_KHR_ray_query): enabled ONLY
    // when requested (RenderVKRayTracing) AND the device supports the extensions
    // and feature bits. Chained in front of f12 below when enabled.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR fAs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR fRq{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceOpacityMicromapFeaturesEXT fOmm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};

    // The always-enabled extensions are part of the requirements check (#526): one list.
    std::vector<const char*> devExts = vkRequiredDeviceExtensions(wantSwapchain, mApi12);
    for (const char* e : devExtsApi12) devExts.push_back(e);
    if (wantEncode)
        for (const char* e : GMDXReplayEncoder::deviceExtensions()) devExts.push_back(e);
    {   // VRAM accounting: real per-heap budget/usage when the driver exposes it.
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool as = false, rq = false, dho = false, omm = false;
        for (const auto& e : ep) {
            if (std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) mMemBudgetExt = true;
            if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) as = true;
            if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) rq = true;
            if (std::strcmp(e.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) == 0) dho = true;
            if (std::strcmp(e.extensionName, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) == 0) omm = true;
            if (std::strcmp(e.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) == 0) sFaultExtListed = true;
            if (std::strcmp(e.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) == 0) sCrumbNVListed = true;
            if (std::strcmp(e.extensionName, VK_AMD_BUFFER_MARKER_EXTENSION_NAME) == 0) sCrumbAMDListed = true;
        }
        if (mMemBudgetExt) devExts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        // #919: the caller's optional extensions (requestOptionalExtension), where listed.
        mOptionalExtsEnabled.clear();
        for (const std::string& want : mOptionalExtsRequested) {
            bool listed = false;
            for (const auto& e : ep)
                if (want == e.extensionName) { listed = true; break; }
            bool dup = false;
            for (const std::string& have : mOptionalExtsEnabled)
                if (have == want) { dup = true; break; }
            if (listed && !dup) mOptionalExtsEnabled.push_back(want);
            VK_LOG("optional device extension %s: %s", want.c_str(), listed ? "enabled" : "not listed by the device");
        }
        // (mOptionalExtsEnabled is not modified again, so these c_str() stay valid for vkCreateDevice)
        for (const std::string& e : mOptionalExtsEnabled) devExts.push_back(e.c_str());
        // #870: the late provider's extensions (all or nothing; de-duplicated against the list).
        mLateExts.clear();
        mLateExtsOn = false;
        mLateExtsWhy = "no provider";
        if (mLateExtProvider) {
            std::vector<std::string> want;
            std::string why;
            if (!mLateExtProvider(mInstance, mPhysical, want, why)) {
                mLateExtsWhy = why.empty() ? std::string("declined") : why;
            } else {
                std::string missing;
                for (const std::string& w : want) {
                    bool listed = false;
                    for (const auto& e : ep)
                        if (w == e.extensionName) { listed = true; break; }
                    if (!listed) missing += (missing.empty() ? "" : ", ") + w;
                }
                if (!missing.empty()) {
                    mLateExtsWhy = "the device does not list " + missing;
                } else {
                    mLateExts = want;   // (not modified again before vkCreateDevice: c_str() stays valid)
                    mLateExtsOn = true;
                    mLateExtsWhy.clear();
                    for (const std::string& w : mLateExts) {
                        bool dup = false;
                        for (const char* have : devExts)
                            if (w == have) { dup = true; break; }
                        if (!dup) devExts.push_back(w.c_str());
                    }
                }
            }
            VK_LOG("late device extensions (%zu): %s", mLateExts.size(),
                   mLateExtsOn ? "enabled" : mLateExtsWhy.c_str());
        }
        if (!gVkBreadcrumbsWanted) { sCrumbNVListed = false; sCrumbAMDListed = false; }
        if (sCrumbNVListed) devExts.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        if (sCrumbAMDListed) devExts.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);

        // Feature bits: the extensions being listed is not enough -- the device must
        // also report accelerationStructure + rayQuery (and core bufferDeviceAddress).
        bool featOk = false;
        std::string rtWhy;
        if (!as) rtWhy += " VK_KHR_acceleration_structure";
        if (!rq) rtWhy += " VK_KHR_ray_query";
        if (!dho) rtWhy += " VK_KHR_deferred_host_operations";
        if (!rtWhy.empty()) rtWhy = "missing extension(s):" + rtWhy;
        if (as && rq && dho) {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR qAs{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayQueryFeaturesKHR qRq{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            VkPhysicalDeviceVulkan12Features q12{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            qAs.pNext = &qRq; qRq.pNext = &q12;
            VkPhysicalDeviceFeatures2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q2.pNext = &qAs;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q2);
            featOk = qAs.accelerationStructure && qRq.rayQuery && q12.bufferDeviceAddress;
            if (!featOk) {
                rtWhy = "missing feature(s):";
                if (!qAs.accelerationStructure) rtWhy += " accelerationStructure";
                if (!qRq.rayQuery) rtWhy += " rayQuery";
                if (!q12.bufferDeviceAddress) rtWhy += " bufferDeviceAddress";
            }
        }
        mRayQuerySupported = as && rq && dho && featOk;
        mRayTracingEnabled = mRayTracingRequested && mRayQuerySupported;
        mRTStatus = mRayTracingEnabled   ? std::string("enabled")
                  : !mRayQuerySupported ? "unsupported, " + rtWhy
                                        : std::string("off (RenderVKRayTracing=0)");
        if (mRayTracingEnabled) {
            devExts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            devExts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            devExts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            fAs.accelerationStructure = VK_TRUE;
            fRq.rayQuery = VK_TRUE;
            fAs.pNext = &fRq;
            fRq.pNext = &f12;
            f2.pNext = &fAs;
            // Opacity micromaps (#194): hardware-resolved alpha-mask cut-outs for rays
            // (RTX 40/50 accelerate them). Optional: RT runs the shader alpha test without.
            if (omm) {
                VkPhysicalDeviceOpacityMicromapFeaturesEXT qo{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT};
                VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                q.pNext = &qo;
                vkGetPhysicalDeviceFeatures2(mPhysical, &q);
                if (qo.micromap) {
                    devExts.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
                    fOmm.micromap = VK_TRUE;
                    fOmm.pNext = f2.pNext;
                    f2.pNext = &fOmm;
                    mOmmEnabled = true;
                }
            }
        }
    }

    // GPU crash diagnostics: VK_EXT_device_fault lets a DEVICE_LOST report the
    // faulting address / vendor info (vkReportDeviceLost).
    VkPhysicalDeviceFaultFeaturesEXT fFault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (sFaultExtListed) {
        VkPhysicalDeviceFaultFeaturesEXT qf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &qf;
        vkGetPhysicalDeviceFeatures2(mPhysical, &q);
        if (qf.deviceFault) {
            fFault.deviceFault = VK_TRUE;
            fFault.pNext = f2.pNext;
            f2.pNext = &fFault;
            devExts.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            sFaultEnabled = true;
        }
    }

    // Per-pipeline robustness (#546): the built-in effects stack runs USER shaders; their
    // pipelines ask for robust buffer / image access (out-of-bounds reads return zero
    // instead of faulting). Only those pipelines: the world stays on the device default.
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fRob{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool pr = false, r2 = false;
        for (const auto& e : ep)
        {
            if (std::strcmp(e.extensionName, VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME) == 0) pr = true;
            if (std::strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) r2 = true;
        }
        if (pr)
        {
            VkPhysicalDevicePipelineRobustnessFeaturesEXT qp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
            VkPhysicalDeviceRobustness2FeaturesEXT q2r{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
            VkPhysicalDeviceVulkan13Features q13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            // #675: the 1.2 path reads robustImageAccess from VK_EXT_image_robustness (queried
            // only; like the 1.3 path it is not enabled -- pipeline robustness needs it supported).
            VkPhysicalDeviceImageRobustnessFeaturesEXT qIr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES_EXT};
            bool irListed = false;
            if (mApi12)
                for (const auto& e : ep)
                    if (std::strcmp(e.extensionName, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME) == 0) irListed = true;
            if (!mApi12) {
                qp.pNext = &q13;
                if (r2) q13.pNext = &q2r;
            } else if (irListed) {
                qp.pNext = &qIr;
                if (r2) qIr.pNext = &q2r;
            } else if (r2) {
                qp.pNext = &q2r;
            }
            q.pNext = &qp;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            const VkBool32 robustImg = mApi12 ? qIr.robustImageAccess : q13.robustImageAccess;
            if (qp.pipelineRobustness)
            {
                fRob.pipelineRobustness = VK_TRUE;
                fRob.pNext = f2.pNext;
                f2.pNext = &fRob;
                devExts.push_back(VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME);
                mPipeRobust = true;
                mRobustBuffer = (r2 && q2r.robustBufferAccess2) ? VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT
                              : q.features.robustBufferAccess ? VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT
                                                              : VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
                mRobustImage = (r2 && q2r.robustImageAccess2) ? VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT
                             : robustImg ? VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_EXT
                                                     : VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
            }
        }
    }

    // DEBUG ROBUSTNESS (#845 / #917, RenderVKDebugRobustness; restart). Device-wide, so every
    // engine pipeline gets it (the per-pipeline robustness above is for the effects stack only):
    // robustBufferAccess (core) + VK_EXT_robustness2 robustBufferAccess2 / robustImageAccess2 /
    // nullDescriptor where supported. A diagnostic: bounds checks cost some shader throughput.
    // Not covered by it: raw device addresses (buffer_reference / AS build inputs) and memory
    // that was freed under a live descriptor -- a loss that persists with it on points there.
    VkPhysicalDeviceRobustness2FeaturesEXT fR2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    mDebugRobustOn = false;
    mDebugRobustStatus = mDebugRobustReq ? "requested" : "off (RenderVKDebugRobustness=0)";
    if (mDebugRobustReq)
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool r2 = false, r2Listed = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) r2 = true;
        for (const char* x : devExts)
            if (x && std::strcmp(x, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0) r2Listed = true;
        VkPhysicalDeviceRobustness2FeaturesEXT q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (r2) q.pNext = &q2;
        vkGetPhysicalDeviceFeatures2(mPhysical, &q);
        if (q.features.robustBufferAccess)
        {
            f2.features.robustBufferAccess = VK_TRUE;
            mDebugRobustOn = true;
        }
        // robustBufferAccess2 requires robustBufferAccess; robustImageAccess2 and nullDescriptor
        // stand alone.
        if (r2 && !r2Listed && (q2.robustImageAccess2 || q2.nullDescriptor
                                || (q2.robustBufferAccess2 && q.features.robustBufferAccess)))
        {
            fR2.robustBufferAccess2 = (q2.robustBufferAccess2 && q.features.robustBufferAccess) ? VK_TRUE : VK_FALSE;
            fR2.robustImageAccess2 = q2.robustImageAccess2;
            fR2.nullDescriptor = q2.nullDescriptor;
            fR2.pNext = f2.pNext;
            f2.pNext = &fR2;
            devExts.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
            mDebugRobustOn = true;
        }
        char rb[192];
        std::snprintf(rb, sizeof(rb), "%s (robustBufferAccess %d, VK_EXT_robustness2 %s: robustBufferAccess2 %d, "
                      "robustImageAccess2 %d, nullDescriptor %d)",
                      mDebugRobustOn ? "ON" : "requested but unsupported",
                      (int)f2.features.robustBufferAccess, r2 ? "supported" : "not supported",
                      (int)fR2.robustBufferAccess2, (int)fR2.robustImageAccess2, (int)fR2.nullDescriptor);
        mDebugRobustStatus = rb;
    }
    VK_LOG("debug robustness (RenderVKDebugRobustness): %s", mDebugRobustStatus.c_str());

    // Variable rate shading (VK_KHR_fragment_shading_rate): per-pipeline coarse rates
    // for the alpha-blend foliage pass (RenderVKAlphaShadingRate). Pipeline rates 1x1,
    // 1x2, 2x1 and 2x2 are guaranteed whenever pipelineFragmentShadingRate is.
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fVrs{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool vrs = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0) vrs = true;
        if (vrs) {
            VkPhysicalDeviceFragmentShadingRateFeaturesKHR qv{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qv;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            if (qv.pipelineFragmentShadingRate) {
                fVrs.pipelineFragmentShadingRate = VK_TRUE;
                // VRS tier 2 (opaque lit pass, RenderVKOpaqueShadingRate): the rate
                // ATTACHMENT, when the device has it, a square texel size the rate compute
                // handles (8..32 px; 16 preferred), R8_UINT is storage-writable and usable
                // as the attachment, and r8ui storage images are allowed in shaders.
                if (qv.attachmentFragmentShadingRate) {
                    VkPhysicalDeviceFragmentShadingRatePropertiesKHR pr{
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
                    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                    p2.pNext = &pr;
                    vkGetPhysicalDeviceProperties2(mPhysical, &p2);
                    VkFormatProperties fp{};
                    vkGetPhysicalDeviceFormatProperties(mPhysical, VK_FORMAT_R8_UINT, &fp);
                    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT
                                                    | VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
                    VkPhysicalDeviceFeatures sup{};
                    vkGetPhysicalDeviceFeatures(mPhysical, &sup);
                    const VkExtent2D lo = pr.minFragmentShadingRateAttachmentTexelSize;
                    const VkExtent2D hi = pr.maxFragmentShadingRateAttachmentTexelSize;
                    uint32_t t = 0;
                    for (uint32_t c : {16u, 8u, 32u})
                        if (c >= lo.width && c <= hi.width && c >= lo.height && c <= hi.height) { t = c; break; }
                    if (t && (fp.optimalTilingFeatures & need) == need && sup.shaderStorageImageExtendedFormats) {
                        fVrs.attachmentFragmentShadingRate = VK_TRUE;
                        f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
                        mVrsImageEnabled = true;
                        mVrsTexel = t;
                    }
                }
                fVrs.pNext = f2.pNext;
                f2.pNext = &fVrs;
                devExts.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                mVrsEnabled = true;
            }
        }
    }

    // Fragment-shader barycentrics (VK_KHR_fragment_shader_barycentric, #664): the RT
    // terminator triangle pass reads the three vertices of the rasterized triangle
    // (pervertexEXT inputs). Optional: without it the RT terminator lift keeps its
    // camera-ray probe (clean_shadow_rt.glsl rtTerminatorProbe).
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR fBary{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    mFragBaryEnabled = false;
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool listed = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0) listed = true;
        if (listed) {
            VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR qb{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qb;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            if (qb.fragmentShaderBarycentric) {
                fBary.fragmentShaderBarycentric = VK_TRUE;
                fBary.pNext = f2.pNext;
                f2.pNext = &fBary;
                devExts.push_back(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
                mFragBaryEnabled = true;
            }
        }
    }

    // PIPELINE EXECUTABLE STATISTICS (RenderVKPipelineStats, read at startup): per-pipeline
    // register / instruction / spill counts from the driver (vk_pipeline.h
    // vkSetPipelineStatsCapture). Only when asked for: nothing changes otherwise.
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR fPipeExec{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    mPipelineStatsEnabled = false;
    if (mPipelineStatsRequested)
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool listed = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) == 0) listed = true;
        if (listed) {
            VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR qp{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qp;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            if (qp.pipelineExecutableInfo) {
                fPipeExec.pipelineExecutableInfo = VK_TRUE;
                fPipeExec.pNext = f2.pNext;
                f2.pNext = &fPipeExec;
                devExts.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
                mPipelineStatsEnabled = true;
            }
        }
        if (!mPipelineStatsEnabled)
            VK_LOG("pipeline statistics (RenderVKPipelineStats): VK_KHR_pipeline_executable_properties %s",
                   listed ? "has no pipelineExecutableInfo" : "not supported by this device");
    }

    // SHADER CLOCK (RenderVKShaderClockLighting, read at startup): the subgroup clock the
    // deferred lighting's instrumented kernels read (clean_shader_clock.glsl). Only when asked
    // for: the production kernels carry no clock capability, so nothing changes otherwise.
    VkPhysicalDeviceShaderClockFeaturesKHR fClock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
    mShaderClockEnabled = false;
    if (mShaderClockRequested)
    {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool listed = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_KHR_SHADER_CLOCK_EXTENSION_NAME) == 0) listed = true;
        if (listed) {
            VkPhysicalDeviceShaderClockFeaturesKHR qc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qc;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            if (qc.shaderSubgroupClock) {
                fClock.shaderSubgroupClock = VK_TRUE;
                fClock.pNext = f2.pNext;
                f2.pNext = &fClock;
                devExts.push_back(VK_KHR_SHADER_CLOCK_EXTENSION_NAME);
                mShaderClockEnabled = true;
            }
        }
        VK_LOG("shader clock (RenderVKShaderClockLighting): %s",
               mShaderClockEnabled ? "VK_KHR_shader_clock shaderSubgroupClock enabled"
               : listed ? "VK_KHR_shader_clock has no shaderSubgroupClock -- off"
                        : "VK_KHR_shader_clock not supported by this device -- off");
    }

    // VRAM accounting (#407): VK_EXT_device_memory_report -- the driver tells us what it
    // allocates for itself (device / queue / pipelines / descriptor + command memory ...)
    // and for every vkAllocateMemory, by object type. Diagnostic only; optional.
    VkPhysicalDeviceDeviceMemoryReportFeaturesEXT fMemRep{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT};
    VkDeviceDeviceMemoryReportCreateInfoEXT memRepCI{
        VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT};
    mDrvReport = false;
    if (gVkMemoryReportWanted) {
        uint32_t en = 0;
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> ep(en);
        vkEnumerateDeviceExtensionProperties(mPhysical, nullptr, &en, ep.data());
        bool listed = false;
        for (const auto& e : ep)
            if (std::strcmp(e.extensionName, VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME) == 0) listed = true;
        if (listed) {
            VkPhysicalDeviceDeviceMemoryReportFeaturesEXT qm{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            q.pNext = &qm;
            vkGetPhysicalDeviceFeatures2(mPhysical, &q);
            if (qm.deviceMemoryReport) {
                fMemRep.deviceMemoryReport = VK_TRUE;
                fMemRep.pNext = f2.pNext;
                f2.pNext = &fMemRep;
                devExts.push_back(VK_EXT_DEVICE_MEMORY_REPORT_EXTENSION_NAME);
                memRepCI.pfnUserCallback = deviceMemoryReportCb;
                memRepCI.pUserData = this;
                memRepCI.pNext = &f2;
                mDrvReport = true;
            }
        }
    }

    // CLUSTER LOD (#743): optional; spliced last (at the head of f2's chain) so a refusal
    // can be retried without them.
    ClusterFeatureChain clusterChain;
    void* const preClusterHead = f2.pNext;
    size_t preClusterExts = devExts.size();   // (<GMDX> lowered when the encode retry drops the video names)
    const VkBool32 preInt64 = f2.features.shaderInt64, preInt64Atomics = f12.shaderBufferInt64Atomics;
    // RenderVKClusterLOD on at startup: every one; off: the inert ones unless
    // RenderVKClusterDeviceFeatures is 0 (then the device is created exactly as before #743).
    if (mClusterRequested || mClusterDeviceFeatures >= 1)
        spliceClusterFeatures(mPhysical, mApi12, mRayTracingEnabled, mClusterRequested, mClusterDeviceFeatures >= 2,
                              clusterChain, devExts, f2, f12, mCluster);
    else
        mCluster = VkClusterFeatures{};
    mCluster.deviceFeatures = mClusterDeviceFeatures;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = mDrvReport ? static_cast<const void*>(&memRepCI) : static_cast<const void*>(&f2);
    dci.queueCreateInfoCount = nqci;
    dci.pQueueCreateInfos = qcis;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    {   // #526: a driver that refuses the device despite the requirements check (or a
        // broken install) is reported to the caller -- message box + clean exit, no crash.
        VkResult dr = vkCreateDevice(mPhysical, &dci, nullptr, &mDevice);
        if (dr != VK_SUCCESS && wantEncode)
        {   // <GMDX> never fail the device over the instant replay encoder: retry without it
            std::fprintf(stderr, "[vkgfx] vkCreateDevice with the video encode queue failed (%s); retrying without\n",
                         vkResultString(dr));
            std::vector<const char*> noEnc;
            for (size_t i = 0; i < devExts.size(); ++i)
            {
                bool video = false;
                for (const char* e : GMDXReplayEncoder::deviceExtensions()) video = video || std::strcmp(devExts[i], e) == 0;
                if (!video) noEnc.push_back(devExts[i]);
            }
            preClusterExts -= devExts.size() - noEnc.size();   // (the video names all precede the cluster ones)
            devExts.swap(noEnc);
            dci.queueCreateInfoCount = --nqci;
            dci.enabledExtensionCount = (uint32_t)devExts.size();
            dci.ppEnabledExtensionNames = devExts.data();
            mDevice = VK_NULL_HANDLE;
            dr = vkCreateDevice(mPhysical, &dci, nullptr, &mDevice);
            encFam = ~0u;
        }
        if (dr != VK_SUCCESS && (devExts.size() != preClusterExts || f2.pNext != preClusterHead))
        {   // #743: never fail the device over an optional cluster LOD capability.
            std::fprintf(stderr, "[vkgfx] vkCreateDevice with the cluster LOD extensions failed (%s); retrying without\n",
                         vkResultString(dr));
            devExts.resize(preClusterExts);
            f2.pNext = preClusterHead;
            f2.features.shaderInt64 = preInt64;
            f12.shaderBufferInt64Atomics = preInt64Atomics;
            mCluster = VkClusterFeatures{};
            mCluster.retried = true;
            mCluster.deviceFeatures = mClusterDeviceFeatures;
            dci.enabledExtensionCount = (uint32_t)devExts.size();
            dci.ppEnabledExtensionNames = devExts.data();
            mDevice = VK_NULL_HANDLE;
            dr = vkCreateDevice(mPhysical, &dci, nullptr, &mDevice);
        }
        if (dr != VK_SUCCESS && mLateExtsOn)
        {   // #870: never fail the device over the late provider's optional extensions either.
            std::fprintf(stderr, "[vkgfx] vkCreateDevice with the late optional extensions failed (%s); retrying without\n",
                         vkResultString(dr));
            std::vector<const char*> keep;
            for (const char* e : devExts) {
                bool late = false;
                for (const std::string& w : mLateExts)
                    if (e == w.c_str()) { late = true; break; }
                if (!late) keep.push_back(e);
            }
            devExts.swap(keep);
            mLateExtsOn = false;
            mLateExtsWhy = std::string("vkCreateDevice refused them (") + vkResultString(dr) + ")";
            dci.enabledExtensionCount = (uint32_t)devExts.size();
            dci.ppEnabledExtensionNames = devExts.data();
            mDevice = VK_NULL_HANDLE;
            dr = vkCreateDevice(mPhysical, &dci, nullptr, &mDevice);
        }
        if (dr != VK_SUCCESS) {
            mDevice = VK_NULL_HANDLE;
            mReqReport.failure = std::string("vkCreateDevice: ") + vkResultString(dr);
            std::fprintf(stderr, "[vkgfx] vkCreateDevice failed: %s\n", vkResultString(dr));
            std::fflush(stderr);
            return false;
        }
    }
    {   // #675: route the 1.3 core names through the KHR / EXT entry points on the 1.2 path
        // (a no-op restore of the core defaults on the 1.3 path).
        const char* missingFn = nullptr;
        if (!vkCompatInstall(mDevice, mApi12, mMaint4, &missingFn)) {
            mReqReport.failure = std::string("the driver did not provide ") + (missingFn ? missingFn : "an entry point");
            std::fprintf(stderr, "[vkgfx] %s\n", mReqReport.failure.c_str());
            std::fflush(stderr);
            vkDestroyDevice(mDevice, nullptr);
            mDevice = VK_NULL_HANDLE;
            return false;
        }
    }
    measureAllocGranularity();   // #407: before the first tracked allocation
    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);
    if (wantEncode && encFam != ~0u)   // <GMDX>
    {
        vkGetDeviceQueue(mDevice, encFam, 0, &mEncodeQueue);
        if (mEncodeQueue) mEncodeFamily = encFam;
        VK_LOG("instant replay: H.264 encode queue %s (family %u)", mEncodeQueue ? "ready" : "unavailable", encFam);
    }
    mComputeQueue = VK_NULL_HANDLE;
    mConcurrent = false;
    if (mComputeFamily != ~0u) {
        vkGetDeviceQueue(mDevice, mComputeFamily, mComputeQueueIndex, &mComputeQueue);
        // Different families: CONCURRENT sharing on every resource (no ownership
        // transfers). Same family: EXCLUSIVE needs no transfer between its queues.
        mConcurrent = mComputeQueue && mComputeFamily != mQueueFamily;
        mShareFamilies[0] = mQueueFamily;
        mShareFamilies[1] = mComputeFamily;
        if (!mComputeQueue) { mComputeFamily = ~0u; mComputeOnly = false; mAsyncStatus = "unavailable (vkGetDeviceQueue)"; }
    }
    VK_LOG("async compute queue: %s%s", mAsyncStatus.c_str(),
           mConcurrent ? " -- buffers / images CONCURRENT across both families" : "");
    sFaultDevice = mDevice;
    sCrumbQueue = mQueue;
    sCrumbQueue2 = mComputeQueue;
    if (sCrumbNVListed)
    {
        sCmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(mDevice, "vkCmdSetCheckpointNV");
        sGetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(mDevice, "vkGetQueueCheckpointDataNV");
        if (!sCmdSetCheckpointNV || !sGetQueueCheckpointDataNV) sCmdSetCheckpointNV = nullptr, sGetQueueCheckpointDataNV = nullptr;
    }
    if (sCrumbAMDListed)
        sCmdWriteBufferMarkerAMD = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(mDevice, "vkCmdWriteBufferMarkerAMD");
    if (sFaultEnabled)
        sGetFaultInfo = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(mDevice, "vkGetDeviceFaultInfoEXT");
    if (mPipelineStatsEnabled)
    {
        auto gp = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(mDevice, "vkGetPipelineExecutablePropertiesKHR");
        auto gs = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(mDevice, "vkGetPipelineExecutableStatisticsKHR");
        if (gp && gs)
        {
            vkSetPipelineStatsCapture(gp, gs);
            VK_LOG("pipeline statistics (RenderVKPipelineStats): ON -- every pipeline is created with "
                   "CAPTURE_STATISTICS and logged (VKPipeStats lines); creation may be slower / miss the cache");
        }
        else mPipelineStatsEnabled = false;
    }
    if (mVrsEnabled)
    {
        mCmdSetShadingRate = (PFN_vkCmdSetFragmentShadingRateKHR)vkGetDeviceProcAddr(mDevice, "vkCmdSetFragmentShadingRateKHR");
        if (!mCmdSetShadingRate) mVrsEnabled = mVrsImageEnabled = false;
    }

    if (mRayTracingEnabled) {
        // Extension entry points are not exported by the loader: resolve them per device.
        auto& f = mRT;
#define VKRT_LOAD(name) f.name = (PFN_##name)vkGetDeviceProcAddr(mDevice, #name)
        VKRT_LOAD(vkCreateAccelerationStructureKHR);
        VKRT_LOAD(vkDestroyAccelerationStructureKHR);
        VKRT_LOAD(vkGetAccelerationStructureBuildSizesKHR);
        VKRT_LOAD(vkCmdBuildAccelerationStructuresKHR);
        VKRT_LOAD(vkGetAccelerationStructureDeviceAddressKHR);
        VKRT_LOAD(vkCmdWriteAccelerationStructuresPropertiesKHR);
        VKRT_LOAD(vkCmdCopyAccelerationStructureKHR);
        if (mOmmEnabled) {
            VKRT_LOAD(vkCreateMicromapEXT);
            VKRT_LOAD(vkDestroyMicromapEXT);
            VKRT_LOAD(vkCmdBuildMicromapsEXT);
            VKRT_LOAD(vkGetMicromapBuildSizesEXT);
            if (!f.vkCreateMicromapEXT || !f.vkDestroyMicromapEXT || !f.vkCmdBuildMicromapsEXT ||
                !f.vkGetMicromapBuildSizesEXT) {
                mOmmEnabled = false;
                f.vkCreateMicromapEXT = nullptr; f.vkDestroyMicromapEXT = nullptr;
                f.vkCmdBuildMicromapsEXT = nullptr; f.vkGetMicromapBuildSizesEXT = nullptr;
            }
        }
#undef VKRT_LOAD
        if (!f.vkCreateAccelerationStructureKHR || !f.vkDestroyAccelerationStructureKHR ||
            !f.vkGetAccelerationStructureBuildSizesKHR || !f.vkCmdBuildAccelerationStructuresKHR ||
            !f.vkGetAccelerationStructureDeviceAddressKHR ||
            !f.vkCmdWriteAccelerationStructuresPropertiesKHR || !f.vkCmdCopyAccelerationStructureKHR) {
            std::fprintf(stderr, "RT: acceleration-structure entry points missing; ray tracing disabled\n");
            mRayTracingEnabled = false;
            mRT = RTFuncs{}; mOmmEnabled = false;
            mRTStatus = "unsupported, acceleration-structure entry points missing";
        } else {
            // #845 / #917: every AS build goes through vkBuildAsNoted, which records its input
            // addresses (vertex / index / transform / instance / AABB / scratch) for the
            // device-lost report's check against the address registry.
            sRealBuildAs = f.vkCmdBuildAccelerationStructuresKHR;
            f.vkCmdBuildAccelerationStructuresKHR = &vkBuildAsNoted;
            VkPhysicalDeviceAccelerationStructurePropertiesKHR ap{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &ap;
            vkGetPhysicalDeviceProperties2(mPhysical, &p2);
            mAsScratchAlign = ap.minAccelerationStructureScratchOffsetAlignment
                                ? ap.minAccelerationStructureScratchOffsetAlignment : 256;
            if (mOmmEnabled) {
                VkPhysicalDeviceOpacityMicromapPropertiesEXT op{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 q2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                q2.pNext = &op;
                vkGetPhysicalDeviceProperties2(mPhysical, &q2);
                mOmmMaxLevel4 = op.maxOpacity4StateSubdivisionLevel;
                if (mOmmMaxLevel4 == 0) mOmmEnabled = false;
            }
        }
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = mQueueFamily;
    VK_CHECK(vkCreateCommandPool(mDevice, &pci, nullptr, &mCommandPool));
    initTexturePools();   // #411: after the granularity (small classes) and the sharing mode

    if (sCmdWriteBufferMarkerAMD)
    {
        sCrumbBuf = createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!sCrumbBuf.mapped) { sCmdWriteBufferMarkerAMD = nullptr; }
        else std::memset(sCrumbBuf.mapped, 0, 16);
    }
    std::fprintf(stdout, "[vkgfx] GPU breadcrumbs: %s\n",
                 sCmdSetCheckpointNV ? "NV checkpoints" : sCmdWriteBufferMarkerAMD ? "AMD buffer markers" : "none");
    return true;
}

// #526: false = this computer can't run the renderer (requirementsReport() says why);
// the partially created objects are released by shutdown().
bool VkDeviceContext::optionalExtensionEnabled(const char* name) const {
    if (!name || mDevice == VK_NULL_HANDLE) return false;
    for (const std::string& e : mOptionalExtsEnabled)
        if (e == name) return true;
    return false;
}

bool VkDeviceContext::init(bool enableValidation) {
    mValidation = enableValidation;
    if (!createInstance(enableValidation, {})) return false;
    if (!pickPhysicalDevice(false)) return false;
    if (!createLogicalDevice(false)) return false;
    createPipelineCache();
    return true;
}

bool VkDeviceContext::initForPresent(bool enableValidation,
        const std::vector<const char*>& extraInstanceExts,
        bool wantSwapchain) {
    mValidation = enableValidation;
    if (!createInstance(enableValidation, extraInstanceExts)) return false;
    if (!pickPhysicalDevice(wantSwapchain)) return false;
    if (!createLogicalDevice(wantSwapchain)) return false;
    createPipelineCache();
    return true;
}

// THE process-wide pipeline cache (vk_pipecache.h): seeded from the validated disk file
// when there is one, else empty. Created without EXTERNALLY_SYNCHRONIZED, so the async
// compile threads (vk_pipeline.cpp) and the main thread share it.
void VkDeviceContext::createPipelineCache() {
    std::vector<uint8_t> blob;
    const bool loaded = vkpipecache::load(mPhysical, mPipeCachePath, mPipeCacheTag, blob);
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    bool seeded = loaded;
    if (vkCreatePipelineCache(mDevice, &ci, nullptr, &mPipeCache) != VK_SUCCESS) {
        mPipeCache = VK_NULL_HANDLE;
        seeded = false;
        if (loaded) {
            // The driver rejected the data: start empty (the next save overwrites it).
            std::fprintf(stderr, "[vkgfx] pipeline cache: driver rejected the disk cache; starting empty\n");
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            if (vkCreatePipelineCache(mDevice, &ci, nullptr, &mPipeCache) != VK_SUCCESS)
                mPipeCache = VK_NULL_HANDLE;
        }
    }
    mPipeCacheBytes = (loaded && mPipeCache) ? blob.size() : 0;
    mPipeCacheWarm = seeded && mPipeCache && !blob.empty();
    vkpipecache::setShared(mPipeCache);
}

bool VkDeviceContext::savePipelineCache(const char* why) {
    if (!mPipeCache || !mDevice) return false;
    if (vkDeviceIsLost()) return false;   // never persist cache data read from a lost device
    return vkpipecache::save(mDevice, mPhysical, mPipeCache, mPipeCachePath, mPipeCacheTag,
                             mPipeCacheBytes, why);
}

void VkDeviceContext::destroyPipelineCache() {
    if (!mPipeCache) return;
    if (vkDeviceIsLost()) { vkpipecache::setShared(VK_NULL_HANDLE); mPipeCache = VK_NULL_HANDLE; return; }
    savePipelineCache("shutdown");   // pipelines created after the last save (FSR, debug views...)
    vkpipecache::setShared(VK_NULL_HANDLE);
    vkDestroyPipelineCache(mDevice, mPipeCache, nullptr);
    mPipeCache = VK_NULL_HANDLE;
}

void VkDeviceContext::shutdown() {
    if (sCrumbBuf.buffer) { destroyBuffer(sCrumbBuf); sCmdWriteBufferMarkerAMD = nullptr; }
    sCmdSetCheckpointNV = nullptr; sGetQueueCheckpointDataNV = nullptr;
    sCmdInsertLabel = nullptr; sDebugUtilsOn = false;   // #973 (createInstance sets them again)
    sCrumbQueue = sCrumbQueue2 = VK_NULL_HANDLE;
    if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
    destroyTexturePools();
    destroyPipelineCache();
    vkSetPipelineStatsCapture(nullptr, nullptr);   // (RenderVKPipelineStats: entry points of this device)
    if (mDevice) vkDestroyDevice(mDevice, nullptr);
    if (mMessenger) {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy) destroy(mInstance, mMessenger, nullptr);
    }
    if (mInstance) vkDestroyInstance(mInstance, nullptr);
    *this = VkDeviceContext{};
}

// ---------------------------------------------------------------------------
// Memory / buffers / images
// ---------------------------------------------------------------------------
// ---- VRAM categories (#407) ----
namespace {
thread_local int t_memCat = -1;   // innermost VkMemScope on this thread (-1: none)
// #697: soft-allocation mode of THIS thread (VkDeviceContext::setSoftAllocFail).
thread_local bool t_softAlloc = false;
// #614: a compaction move's destination is being created on this thread (TexCompactScope):
// the blocks it must avoid. Non-null = pooled into an existing non-empty block or nothing.
thread_local const std::vector<VkDeviceMemory>* t_texCompact = nullptr;

// Category of an allocation made without a VkMemScope, from what it is created for.
uint8_t vkMemInfer(bool image, uint32_t usage, bool host)
{
    if (image)
    {
        if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     | VK_IMAGE_USAGE_STORAGE_BIT))
            return VKMEM_TARGET;
        return VKMEM_TEXTURE;
    }
    if (host)
        return ((usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) && !(usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
               ? VKMEM_READBACK : VKMEM_STAGING;
    if (usage & VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) return VKMEM_RT_AUX;
    if (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return VKMEM_GEOMETRY;
    if (usage & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT) return VKMEM_CULL;
    return VKMEM_SCENE;
}
} // namespace

const char* vkMemCatName(uint32_t cat)
{
    static const char* kName[VKMEM_COUNT] = {"tex", "geom", "scene", "cull", "targets", "probes", "shadow", "mirror",
                                             "rt-as", "rt-aux", "rt-mask", "staging", "readback", "part", "ui",
                                             "upscale", "texenc", "rig", "denoise"};
    return cat < VKMEM_COUNT ? kName[cat] : "?";
}

const char* vkDrvBucketName(uint32_t b)
{
    static const char* kName[VKDRV_COUNT] = {"devmem", "device", "queue", "cmd", "desc", "pipe", "query",
                                             "swapchain", "resource", "accel", "sync", "other"};
    return b < VKDRV_COUNT ? kName[b] : "?";
}

namespace {
uint8_t vkDrvBucket(VkObjectType t)
{
    switch (t)
    {
    case VK_OBJECT_TYPE_DEVICE_MEMORY: return VKDRV_DEVMEM;
    case VK_OBJECT_TYPE_INSTANCE:
    case VK_OBJECT_TYPE_PHYSICAL_DEVICE:
    case VK_OBJECT_TYPE_DEVICE: return VKDRV_DEVICE;
    case VK_OBJECT_TYPE_QUEUE: return VKDRV_QUEUE;
    case VK_OBJECT_TYPE_COMMAND_POOL:
    case VK_OBJECT_TYPE_COMMAND_BUFFER: return VKDRV_CMD;
    case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
    case VK_OBJECT_TYPE_DESCRIPTOR_SET:
    case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
    case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return VKDRV_DESC;
    case VK_OBJECT_TYPE_PIPELINE:
    case VK_OBJECT_TYPE_PIPELINE_CACHE:
    case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
    case VK_OBJECT_TYPE_SHADER_MODULE: return VKDRV_PIPE;
    case VK_OBJECT_TYPE_QUERY_POOL: return VKDRV_QUERY;
    case VK_OBJECT_TYPE_SWAPCHAIN_KHR:
    case VK_OBJECT_TYPE_SURFACE_KHR: return VKDRV_SWAPCHAIN;
    case VK_OBJECT_TYPE_IMAGE:
    case VK_OBJECT_TYPE_BUFFER:
    case VK_OBJECT_TYPE_IMAGE_VIEW:
    case VK_OBJECT_TYPE_BUFFER_VIEW:
    case VK_OBJECT_TYPE_SAMPLER: return VKDRV_RESOURCE;
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR:
    case VK_OBJECT_TYPE_MICROMAP_EXT: return VKDRV_ACCEL;
    case VK_OBJECT_TYPE_FENCE:
    case VK_OBJECT_TYPE_SEMAPHORE:
    case VK_OBJECT_TYPE_EVENT: return VKDRV_SYNC;
    default: return VKDRV_OTHER;
    }
}
} // namespace

void VkDeviceContext::onDriverMemoryReport(const VkDeviceMemoryReportCallbackDataEXT& d)
{
    std::lock_guard<std::mutex> lk(mDrvMutex.m);
    switch (d.type)
    {
    case VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT:
    case VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT:
    {
        const bool import = d.type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT;
        const uint32_t heap = d.heapIndex < VK_MAX_MEMORY_HEAPS ? d.heapIndex : 0u;
        auto it = mDrvObjs.find(d.memoryObjectId);
        if (it != mDrvObjs.end())
        {   // re-reported id (e.g. imported again): replace, never double count
            const DrvRec& o = it->second;
            uint64_t& sum = o.import ? mDrvImport[o.heap] : mDrvHeap[o.heap][o.bucket];
            sum -= std::min(sum, o.bytes);
        }
        const DrvRec r{(uint64_t)d.size, heap, vkDrvBucket(d.objectType), import};
        mDrvObjs[d.memoryObjectId] = r;
        (import ? mDrvImport[heap] : mDrvHeap[heap][r.bucket]) += r.bytes;
        break;
    }
    case VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_FREE_EXT:
    case VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_UNIMPORT_EXT:
    {
        // (size / heapIndex are undefined on these events: the live record has them)
        auto it = mDrvObjs.find(d.memoryObjectId);
        if (it == mDrvObjs.end()) break;
        const DrvRec& o = it->second;
        uint64_t& sum = o.import ? mDrvImport[o.heap] : mDrvHeap[o.heap][o.bucket];
        sum -= std::min(sum, o.bytes);
        mDrvObjs.erase(it);
        break;
    }
    case VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT: ++mDrvFailures; break;
    default: break;
    }
}

// The driver's allocation granularity (#407): every vkAllocateMemory is rounded up to it
// in the process usage VK_EXT_memory_budget reports (NVIDIA: 64 KiB; a 4x4 texture costs
// 64 KiB of VRAM). Measured once, right after device creation, before anything else
// allocates: 16 device-local allocations of 1 MiB + 1 byte on the main heap and the usage
// delta (the size clears small-request suballocators, which would report their chunk
// size; the rounding of the odd byte is the granularity). Falls back to 64 KiB
// ("assumed") without the budget extension or on an implausible delta.
void VkDeviceContext::measureAllocGranularity()
{
    mAllocGran = 64ull << 10;
    mAllocGranMeasured = false;
    if (!mMemBudgetExt || !mDevice) return;
    uint32_t heap = ~0u, type = ~0u;
    for (uint32_t h = 0; h < mMemProps.memoryHeapCount; ++h)
        if ((mMemProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            && (heap == ~0u || mMemProps.memoryHeaps[h].size > mMemProps.memoryHeaps[heap].size))
            heap = h;
    if (heap == ~0u) return;
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount && type == ~0u; ++i)
        if (mMemProps.memoryTypes[i].heapIndex == heap
            && (mMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            && !(mMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type = i;
    if (type == ~0u) return;
    auto usage = [&]() -> uint64_t
    {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        p2.pNext = &bud;
        vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
        return bud.heapUsage[heap];
    };
    constexpr uint32_t kN = 16;
    constexpr VkDeviceSize kSize = (1ull << 20) + 1;
    VkDeviceMemory mem[kN] = {};
    const uint64_t before = usage();
    uint32_t made = 0;
    for (; made < kN; ++made)
    {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = kSize;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(mDevice, &ai, nullptr, &mem[made]) != VK_SUCCESS) break;
    }
    const uint64_t after = usage();
    for (uint32_t i = 0; i < made; ++i) vkFreeMemory(mDevice, mem[i], nullptr);
    if (made < kN || after <= before) return;
    // per-allocation rounding = what each took beyond its size (granularity - 1 byte)
    const uint64_t per = (after - before) / kN;
    if (per <= kSize) return;
    const uint64_t pad = per - kSize + 1;
    if (pad < (4ull << 10) || pad > (2ull << 20)) return;
    uint64_t g = 4ull << 10;
    while (g < pad) g <<= 1;
    mAllocGran = g;
    mAllocGranMeasured = true;
}

VkMemScope::VkMemScope(VkMemCat c) : mPrev(t_memCat) { t_memCat = (int)c; }
VkMemScope::~VkMemScope() { t_memCat = mPrev; }
VkDeviceContext::TexCompactScope::TexCompactScope(const std::vector<VkDeviceMemory>* avoid) : mPrev(t_texCompact)
{
    static const std::vector<VkDeviceMemory> s_none;
    t_texCompact = avoid ? avoid : &s_none;
}
VkDeviceContext::TexCompactScope::~TexCompactScope() { t_texCompact = mPrev; }

void VkDeviceContext::trackAlloc(VkDeviceMemory mem, VkDeviceSize bytes, uint32_t typeIndex, bool image,
                                 uint32_t usage, bool asStorage, const char* site) {
    const uint32_t heap = mMemProps.memoryTypes[typeIndex].heapIndex;
    const bool host = (mMemProps.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    // #695: "dev" = in the budgeted heaps (the DEVICE_LOCAL flag, or on UMA the carve-out +
    // the host heap) -- textures spilled to the host heap stay textures for the ladder.
    // The page padding stays on the raw flag (the granularity is the device heap's).
    const bool devHeap = (mMemProps.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    const bool dev = mUma ? inBudgetHeap(heap) : devHeap;
    bool inferred = false;
    uint8_t cat;
    if (asStorage) cat = VKMEM_RT_AS;
    else if (t_memCat >= 0 && t_memCat < (int)VKMEM_COUNT) cat = (uint8_t)t_memCat;
    else { cat = vkMemInfer(image, usage, host); inferred = true; }
    // Driver page padding (#407): device-local only (system-heap pages are not VRAM).
    const uint64_t g = mAllocGran ? mAllocGran : 1;
    const uint32_t pad = devHeap ? (uint32_t)(((uint64_t)bytes + g - 1) / g * g - (uint64_t)bytes) : 0u;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    mAllocs[mem] = AllocRec{ (uint64_t)bytes, heap, image, host, cat, inferred, dev, pad, inferred ? site : nullptr };
    mCatPad[cat] += pad;
    mPadBytes += pad;
    if (inferred)
    {
        SiteRec& sr = mInferredSites[site];
        sr.bytes += bytes;
        ++sr.count;
        sr.cat = cat;
    }
    mHeapUsed[heap] += bytes;
    (image ? mImgBytes : mBufBytes) += bytes;
    if (host) mHostBytes += bytes;
    // #458: host-visible split + allocation traffic
    if (host) { mCatHV[cat] += bytes; ++mCatHVCount[cat]; ++mHostCount; }
    ++mAllocCalls[host ? 1 : 0];
    mAllocBytesTotal[host ? 1 : 0] += bytes;
    ++mCatAllocCalls[cat];
    if (image && (cat == VKMEM_TEXTURE || cat == VKMEM_UI)) ++mTexDevAllocCalls;   // #411 hitch counter
    (dev ? mCatDev[cat] : mCatHost[cat]) += bytes;
    ++mCatCount[cat];
    mCatPeak[cat] = std::max(mCatPeak[cat], mCatHost[cat] + mCatDev[cat]);
    mCatPeakInt[cat] = std::max(mCatPeakInt[cat], mCatHost[cat] + mCatDev[cat]);
    if (inferred) mInferredBytes += bytes;
    if (dev)
    {
        mDevLocalBytes += bytes;
        mDevLocalPeak = std::max(mDevLocalPeak, mDevLocalBytes);
    }
}

void VkDeviceContext::trackFree(VkDeviceMemory mem) {
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    auto it = mAllocs.find(mem);
    if (it == mAllocs.end()) return;
    const AllocRec& r = it->second;
    mHeapUsed[r.heap] -= r.bytes;
    (r.image ? mImgBytes : mBufBytes) -= r.bytes;
    if (r.host) mHostBytes -= r.bytes;
    if (r.image && (r.cat == VKMEM_TEXTURE || r.cat == VKMEM_UI)) ++mTexDevFreeCalls;   // #411 hitch counter
    if (r.dev) mDevLocalBytes -= r.bytes;
    mPadBytes -= std::min<uint64_t>(mPadBytes, r.pad);
    if (r.pool)
    {   // texture pool block (#411): its bytes were never in a category
        mTexPoolBlockBytes -= std::min(mTexPoolBlockBytes, r.bytes);
        if (mTexPoolBlocks) --mTexPoolBlocks;
        mAllocs.erase(it);
        return;
    }
    (r.dev ? mCatDev[r.cat] : mCatHost[r.cat]) -= r.bytes;
    if (mCatCount[r.cat]) --mCatCount[r.cat];
    // #458 (pool blocks returned above: they never went through trackAlloc)
    if (r.host)
    {
        mCatHV[r.cat] -= std::min(mCatHV[r.cat], r.bytes);
        if (mCatHVCount[r.cat]) --mCatHVCount[r.cat];
        if (mHostCount) --mHostCount;
    }
    ++mFreeCalls[r.host ? 1 : 0];
    mFreeBytesTotal[r.host ? 1 : 0] += r.bytes;
    ++mCatFreeCalls[r.cat];
    if (r.inferred)
    {
        mInferredBytes -= r.bytes;
        auto st = mInferredSites.find(r.site);
        if (st != mInferredSites.end())
        {
            st->second.bytes -= std::min(st->second.bytes, r.bytes);
            if (st->second.count) --st->second.count;
            if (!st->second.count) mInferredSites.erase(st);
        }
    }
    mCatPad[r.cat] -= std::min<uint64_t>(mCatPad[r.cat], r.pad);
    mAllocs.erase(it);
}

std::string VkDeviceContext::texSizeHistogramText() const {
    static constexpr uint64_t kEdge[] = {64ull << 10, 256ull << 10, 1ull << 20, 2ull << 20, 4ull << 20, 8ull << 20};
    static constexpr const char* kName[] = {"<=64K", "64-256K", "256K-1M", "1-2M", "2-4M", "4-8M", ">8M"};
    constexpr int kN = 7;
    struct Band { uint32_t pooled = 0, ded = 0; uint64_t pooledBytes = 0, dedBytes = 0, dedPad = 0; };
    Band b[kN];
    // (exact bytes, {count, where}): where = 's' medium/small slab class, 'f' TLSF fallback
    // pool, 'd' dedicated (#587: shows whether the size classes catch the real sizes).
    struct SizeRec { uint32_t n = 0; char where = 'd'; };
    std::vector<std::pair<uint64_t, SizeRec>> topSizes;
    auto band = [](uint64_t bytes) { int i = 0; while (i < kN - 1 && bytes > kEdge[i]) ++i; return i; };
    const uint64_t g = mAllocGran ? mAllocGran : 1;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        for (const auto& kv : mPooled)
        {
            if (kv.second.cat != VKMEM_TEXTURE) continue;
            Band& x = b[band(kv.second.bytes)];
            ++x.pooled; x.pooledBytes += kv.second.bytes;
        }
        for (const auto& kv : mAllocs)
        {
            const AllocRec& r = kv.second;
            if (!r.image || r.pool || r.cat != VKMEM_TEXTURE) continue;
            Band& x = b[band(r.bytes)];
            ++x.ded; x.dedBytes += r.bytes; x.dedPad += (r.bytes + g - 1) / g * g - r.bytes;
        }
        // Exact memory-requirement sizes above 64 KiB (size-class design): the most common
        // ones, with counts -- the driver's tiling / mip-tail padding decides the real classes.
        std::unordered_map<uint64_t, SizeRec> exact;
        for (const auto& kv : mPooled)
            if (kv.second.cat == VKMEM_TEXTURE && kv.second.bytes > (64ull << 10))
            {
                SizeRec& e = exact[kv.second.bytes];
                ++e.n;
                const bool slab = kv.second.pool < mTexPools.size() && mTexPools[kv.second.pool].classBytes != 0;
                if (e.where != 'f') e.where = slab ? 's' : 'f';   // any fallback image marks the size
            }
        for (const auto& kv : mAllocs)
            if (kv.second.image && !kv.second.pool && kv.second.cat == VKMEM_TEXTURE && kv.second.bytes > (64ull << 10))
                ++exact[kv.second.bytes].n;
        topSizes.assign(exact.begin(), exact.end());
    }
    std::sort(topSizes.begin(), topSizes.end(),
              [](const std::pair<uint64_t, SizeRec>& a, const std::pair<uint64_t, SizeRec>& c)
              { return a.second.n != c.second.n ? a.second.n > c.second.n : a.first < c.first; });
    std::ostringstream os;
    os << "tex size bands (pooled n/MB | dedicated n/MB +pad):";
    for (int i = 0; i < kN; ++i)
    {
        const Band& x = b[i];
        if (!x.pooled && !x.ded) continue;
        os << " " << kName[i] << " " << x.pooled << "/" << (x.pooledBytes >> 20) << " | " << x.ded << "/"
           << (x.dedBytes >> 20) << " +" << (x.dedPad >> 20) << ";";
    }
    // KiB rounded UP: a value copied into RenderVKTexturePoolClassesKB must cover the size.
    os << " | top sizes >64K (KiB x n, s slab / f fallback / d dedicated):";
    const size_t shown = std::min<size_t>(topSizes.size(), 16);
    for (size_t i = 0; i < shown; ++i)
        os << " " << ((topSizes[i].first + 1023) >> 10) << "x" << topSizes[i].second.n << topSizes[i].second.where;
    if (topSizes.size() > shown) os << " (+" << (topSizes.size() - shown) << " other sizes)";
    // Hot sizes the classes miss (#587): many images in the TLSF fallback at one size ->
    // a class (size + ~1.6% headroom) the owner can add.
    std::string sugg;
    for (const auto& t : topSizes)
    {
        if (t.second.where != 'f' || t.second.n < 32) continue;
        if (!sugg.empty()) sugg += ",";
        sugg += std::to_string((unsigned long long)((t.first + t.first / 64 + 1023) >> 10));
        if (sugg.size() > 96) break;
    }
    if (!sugg.empty()) os << " | fallback hot sizes: try RenderVKTexturePoolClassesKB=auto," << sugg;
    return os.str();
}

std::string VkDeviceContext::inferredSitesText(size_t top) const {
    std::vector<std::pair<const char*, SiteRec>> v;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        v.assign(mInferredSites.begin(), mInferredSites.end());
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
    std::string o;
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        if (!o.empty()) o += ", ";
        o += v[i].first ? v[i].first : "?";
        o += " " + std::to_string((unsigned long long)((v[i].second.bytes + (1u << 19)) >> 20)) + "MB/"
           + std::to_string(v[i].second.count) + " " + vkMemCatName(v[i].second.cat);
    }
    return o;
}

bool VkDeviceContext::allocDeviceLocal(VkDeviceMemory mem) const {
    if (!mem) return false;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    auto it = mAllocs.find(mem);
    return it != mAllocs.end() && it->second.dev;
}

uint64_t VkDeviceContext::allocBytes(VkDeviceMemory mem) const {
    if (!mem) return 0;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    auto it = mAllocs.find(mem);
    return it == mAllocs.end() || it->second.pool ? 0 : it->second.bytes;
}

uint64_t VkDeviceContext::allocBytes(VkImage image, VkDeviceMemory mem) const {
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    if (image && !mPooled.empty())
    {
        auto p = mPooled.find(image);
        if (p != mPooled.end()) return p->second.bytes;
    }
    if (!mem) return 0;
    auto it = mAllocs.find(mem);
    return it == mAllocs.end() || it->second.pool ? 0 : it->second.bytes;
}

bool VkDeviceContext::imagePooled(VkImage image) const {
    if (!image) return false;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    return mPooled.find(image) != mPooled.end();
}

void VkDeviceContext::resetCatPeakInterval() {
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c) mCatPeakInt[c] = mCatHost[c] + mCatDev[c];
}

VkDeviceContext::MemStats VkDeviceContext::memStats() const {
    MemStats m;
    // Texture pools (#411): VMA's per-pool view first -- never call VMA under mAllocMutex
    // (its block callbacks take that mutex while VMA holds the pool's lock).
    if (mVma)
    {
        m.texPools = true;
        m.texPoolCount = (uint32_t)std::min<size_t>(mTexPools.size(), MemStats::kTexPoolMax);
        for (uint32_t i = 0; i < m.texPoolCount; ++i)
        {
            VmaStatistics st{};
            vmaGetPoolStatistics(mVma, mTexPools[i].pool, &st);
            MemStats::TexPool& p = m.texPool[i];
            p.classBytes = mTexPools[i].classBytes;
            p.slotsPerBlock = mTexPools[i].slots;
            p.blockSize = mTexPools[i].blockSize;
            p.blocks = st.blockCount;
            p.allocs = st.allocationCount;
            p.blockBytes = st.blockBytes;
            p.roundedBytes = st.allocationBytes;
            m.texPoolRounded += st.allocationBytes;
        }
        m.texSmallMax = mTexSmallMax;
        m.texMediumMax = mTexMediumMax;
    }
    m.heapCount = mMemProps.memoryHeapCount;
    m.budgetExt = mMemBudgetExt;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    if (mMemBudgetExt && mPhysical) {
        VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        p2.pNext = &bud;
        vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
    }
    for (uint32_t h = 0; h < m.heapCount && h < VK_MAX_MEMORY_HEAPS; ++h) {
        m.heapDeviceLocal[h] = (mMemProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        m.heapBudget[h] = mMemBudgetExt ? bud.heapBudget[h] : mMemProps.memoryHeaps[h].size;
        m.heapDriverUsage[h] = mMemBudgetExt ? bud.heapUsage[h] : 0;
    }
    if (mUma)
    {   // #695 (the debug carve-out clamp applies to every reader of the budget)
        m.uma = true;
        m.umaDevHeap = mUmaDevHeap;
        m.umaHostHeap = mUmaHostHeap;
        m.umaHostCeil = mUmaHostCeil;
        if (mUmaFakeCarve && mUmaDevHeap < VK_MAX_MEMORY_HEAPS)
            m.heapBudget[mUmaDevHeap] = std::min(m.heapBudget[mUmaDevHeap], mUmaFakeCarve);
        std::lock_guard<std::mutex> ul(mUmaMutex.m);
        m.umaSpills = mUmaSpills;
        m.umaSpillBytes = mUmaSpillBytes;
    }
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    for (uint32_t h = 0; h < m.heapCount && h < VK_MAX_MEMORY_HEAPS; ++h) m.heapUsed[h] = mHeapUsed[h];
    m.bufferBytes = mBufBytes;
    m.imageBytes = mImgBytes;
    m.hostVisibleBytes = mHostBytes;
    m.allocCount = (uint32_t)mAllocs.size();
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
    {
        m.catDev[c] = mCatDev[c];
        m.catHost[c] = mCatHost[c];
        m.catPeak[c] = mCatPeak[c];
        m.catPeakInt[c] = mCatPeakInt[c];
        m.catCount[c] = mCatCount[c];
    }
    m.inferredBytes = mInferredBytes;
    m.devLocalBytes = mDevLocalBytes;
    m.devLocalPeak = mDevLocalPeak;
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c) m.catPad[c] = mCatPad[c];
    m.padBytes = mPadBytes;
    for (uint32_t i = 0; i < m.texPoolCount; ++i) m.texPool[i].liveBytes = mTexPoolLive[i];
    if (m.texPoolCount)
    {
        // #614: non-empty blocks per pool (our table) vs VMA's block count -> empty blocks
        // (hysteresis) and what a perfect packing would free.
        uint32_t ne[MemStats::kTexPoolMax] = {}, imgs[MemStats::kTexPoolMax] = {};
        uint64_t used[MemStats::kTexPoolMax] = {};
        for (const auto& kv : mTexBlocks)
        {
            const uint32_t p = kv.second.pool;
            if (p >= m.texPoolCount) continue;
            ++ne[p]; imgs[p] += kv.second.images; used[p] += kv.second.used;
        }
        for (uint32_t i = 0; i < m.texPoolCount; ++i)
        {
            MemStats::TexPool& p = m.texPool[i];
            p.emptyBlocks = p.blocks > ne[i] ? p.blocks - ne[i] : 0;
            p.reclaimBytes = texPoolReclaimOf(i, ne[i], imgs[i], used[i]);
            m.texPoolEmptyBlocks += p.emptyBlocks;
            m.texPoolEmptyBytes += (uint64_t)p.emptyBlocks * p.blockSize;
            m.texPoolReclaim += p.reclaimBytes;
        }
    }
    m.texCompactMoves = mTexCompactMoves;
    m.texCompactMoved = mTexCompactMoved;
    m.texCompactSkipped = mTexCompactSkipped;
    m.texCompactEvacuated = mTexCompactEvacuated;
    m.texCompactPlaceFail = mTexCompactPlaceFail;
    m.texCompactReleased = mTexCompactReleased;
    m.texCompactReleasedBytes = mTexCompactReleasedBytes;
    m.texPoolBlocks = mTexPoolBlocks;
    m.texPooledImages = (uint32_t)mPooled.size();
    m.texPoolBlockBytes = mTexPoolBlockBytes;
    m.texPoolLive = mTexPoolLiveTotal;
    m.texPoolSlack = mTexPoolBlockBytes > mTexPoolLiveTotal ? mTexPoolBlockBytes - mTexPoolLiveTotal : 0;
    m.texDedicatedWanted = mTexDedicatedWanted;
    m.texClassMiss = mTexClassMiss;
    m.texClassHit = mTexClassHit;
    m.texMedAlignMax = mTexMedAlignMax;
    m.texDevAllocCalls = mTexDevAllocCalls;
    m.texDevFreeCalls = mTexDevFreeCalls;
    m.texSubAllocCalls = mTexSubAllocCalls;
    m.texSubFreeCalls = mTexSubFreeCalls;
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
    {
        m.catHV[c] = mCatHV[c];
        m.catHVCount[c] = mCatHVCount[c];
        m.catAllocCalls[c] = mCatAllocCalls[c];
        m.catFreeCalls[c] = mCatFreeCalls[c];
    }
    m.hostVisibleCount = mHostCount;
    for (int k = 0; k < 2; ++k)
    {
        m.allocCalls[k] = mAllocCalls[k];
        m.freeCalls[k] = mFreeCalls[k];
        m.allocBytesTotal[k] = mAllocBytesTotal[k];
        m.freeBytesTotal[k] = mFreeBytesTotal[k];
    }
    m.allocGranularity = mAllocGran;
    m.granularityMeasured = mAllocGranMeasured;
    m.drvReport = mDrvReport;
    if (mDrvReport)
    {
        std::lock_guard<std::mutex> dl(mDrvMutex.m);
        std::memcpy(m.drvHeap, mDrvHeap, sizeof(m.drvHeap));
        std::memcpy(m.drvImport, mDrvImport, sizeof(m.drvImport));
        m.drvObjects = (uint32_t)mDrvObjs.size();
        m.drvFailures = mDrvFailures;
    }
    return m;
}

uint64_t VkDeviceContext::deviceLocalBudgetBytes() const {
    const MemStats m = memStats();
    if (m.uma) return budgetHeaps(m).budget;   // #695
    uint64_t best = 0;
    for (uint32_t h = 0; h < m.heapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
        if (m.heapDeviceLocal[h] && m.heapBudget[h] > best) best = m.heapBudget[h];
    return best;
}

uint64_t VkDeviceContext::heapUsedBytes(uint32_t heap) const {
    if (heap >= VK_MAX_MEMORY_HEAPS) return 0;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    return mHeapUsed[heap];
}

void VkDeviceContext::setSoftAllocFail(bool on) { t_softAlloc = on; }
bool VkDeviceContext::softAllocFail() const { return t_softAlloc; }

// True when `r` is an out-of-memory result that the soft-allocation mode absorbs
// (counted; logged at most once a minute). Anything else stays fatal via VK_CHECK.
bool VkDeviceContext::softFail(VkResult r, const char* what, uint64_t bytes) {
    if (!t_softAlloc || (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY))
        return false;
    // #595 #597 #599: counted per site (memory category / call), one WARNING per site per
    // 10 s through the log hook (Slipstream.log; stderr never reached the user's log).
    std::string site = (t_memCat >= 0 && t_memCat < (int)VKMEM_COUNT) ? vkMemCatName((uint32_t)t_memCat) : "untagged";
    site += '/';
    site += what;
    const double now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool log = false;
    uint32_t since = 0, total = 0, all = 0;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        ++mAllocFailures;
        mLastFailBytes = bytes;
        SoftSite& s = mSoftSites[site];
        ++s.count;
        ++s.since;
        s.lastBytes = bytes;
        if (now - s.lastLog >= 10.0)
        {
            log = true;
            s.lastLog = now;
            since = s.since;
            s.since = 0;
        }
        total = s.count;
        all = mAllocFailures;
    }
    if (log)
    {
        uint64_t usage = 0, budget = 0;
        const bool live = liveDeviceBudget(usage, budget);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "VRAM soft-fail [%s]: %llu KB request refused (%s); %u since the last line, %u at this site, "
                      "%u in all; device heap %s%llu MB used of an OS budget of %llu MB -- the caller keeps / defers",
                      site.c_str(), (unsigned long long)(bytes >> 10), vkResultString(r), since, total, all,
                      live ? "" : "(no budget ext) ", (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20));
        allocLog(buf);
    }
    return true;
}

void VkDeviceContext::allocLog(const std::string& s) const
{
    if (mAllocLog) mAllocLog(s);
    else std::fprintf(stderr, "[vk] %s\n", s.c_str());
}

std::string VkDeviceContext::softFailSummary() const
{
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    std::string o = std::to_string(mAllocFailures);
    if (mSoftSites.empty()) return o;
    o += " (sites";
    for (const auto& kv : mSoftSites) o += " " + kv.first + " x" + std::to_string(kv.second.count);
    o += ")";
    return o;
}

bool VkDeviceContext::liveDeviceBudget(uint64_t& usage, uint64_t& budget) const
{
    usage = budget = 0;
    if (!mMemBudgetExt || !mPhysical) return false;
    if (mUma)
    {   // #695: the combined UMA budget, usage of both heaps
        const MemStats m = memStats();
        const BudgetHeaps bh = budgetHeaps(m);
        budget = bh.budget;
        usage = m.heapDriverUsage[bh.heap] + m.heapDriverUsage[bh.hostHeap];
        return budget != 0;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    p2.pNext = &bud;
    vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
    for (uint32_t h = 0; h < mMemProps.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
    {
        if (!(mMemProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) || bud.heapBudget[h] <= budget) continue;
        budget = bud.heapBudget[h];
        usage = bud.heapUsage[h];
    }
    return budget != 0;
}

void VkDeviceContext::setPressureRelief(std::function<uint64_t()> fn)
{
    mRelief = std::move(fn);
    mReliefThread = std::this_thread::get_id();
}

bool VkDeviceContext::runRelief(double min_interval, uint64_t& freed)
{
    freed = 0;
    const double tnow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!mRelief || mInRelief || std::this_thread::get_id() != mReliefThread || tnow - mLastRelief < min_interval)
        return false;
    mLastRelief = tnow;
    mInRelief = true;
    freed = mRelief();
    mInRelief = false;
    return true;
}

VkResult VkDeviceContext::allocateMemory(const VkMemoryAllocateInfo& ai, VkDeviceMemory* mem, const char* what)
{
    *mem = VK_NULL_HANDLE;
    // Debug fault injection (SLVkInjectAllocFail): runtime allocations only.
    if (mRuntimeAllocs && mInjectEvery && (t_softAlloc || mInjectHard))
    {
        uint64_t n = 0;
        {
            std::lock_guard<std::mutex> lk(mAllocMutex.m);
            n = ++mInjectCount;
        }
        if (n % mInjectEvery == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;   // (no relief: the soft path is under test)
    }
    VkResult r = vkAllocateMemory(mDevice, &ai, nullptr, mem);
    if (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY) return r;
    // Out of memory: free what only waits on finished frames (+ the renderer's fast ladder
    // step), then one retry -- hard or soft. (At most every 0.25 s: it idles the device.)
    uint64_t freed = 0;
    // (a hard request is never throttled: the alternative is an abort)
    if (!runRelief(t_softAlloc ? 0.25 : 0.0, freed)) return r;
    const VkResult r2 = vkAllocateMemory(mDevice, &ai, nullptr, mem);
    const double tnow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (tnow - mLastOomRetryLog >= 5.0)
    {
        mLastOomRetryLog = tnow;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "VRAM: %s of %llu KB out of memory; relief freed %llu MB -> retry %s", what,
                      (unsigned long long)(ai.allocationSize >> 10), (unsigned long long)(freed >> 20),
                      r2 == VK_SUCCESS ? "succeeded" : vkResultString(r2));
        allocLog(buf);
    }
    if (r2 != VK_SUCCESS) *mem = VK_NULL_HANDLE;
    return r2;
}

bool VkDeviceContext::heapBudgetPreflight(uint32_t memTypeIndex, uint64_t bytes, const char* what)
{
    // Hard requests: >= 64 MB (the NVIDIA segfault guard, #563). Soft ones: >= 4 MB, and
    // refused once they would pass the CURRENT OS budget (#595 #597 #599: AMD on Windows
    // returns OUT_OF_DEVICE_MEMORY there instead of over-committing) -- not attempted.
    const bool soft = t_softAlloc;
    const uint64_t floor_bytes = soft ? (4ull << 20) : (64ull << 20);
    if (!mMemBudgetExt || bytes < floor_bytes || memTypeIndex >= mMemProps.memoryTypeCount
        || !(mMemProps.memoryTypes[memTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        return true;
    const uint32_t heap = mMemProps.memoryTypes[memTypeIndex].heapIndex;
    auto fits = [&](uint64_t& usage, uint64_t& budget) -> bool
    {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        p2.pNext = &bud;
        vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
        budget = bud.heapBudget[heap];
        usage = bud.heapUsage[heap];
        // >= 64 MB: keep ~3% (at least 256 MB) of the heap budget free for the driver and
        // other allocations. Smaller soft requests: 1.5% (they only must not pass the wall).
        const uint64_t margin = bytes >= (64ull << 20) ? std::max<uint64_t>(budget / 32u, 256ull << 20) : budget / 64u;
        return !budget || usage + bytes + margin <= budget;
    };
    uint64_t usage = 0, budget = 0;
    if (fits(usage, budget)) return true;
    // Over: let the renderer free what is only waiting on finished frames, then re-check.
    // (at most once per 2 s: it idles the device, and under sustained pressure every large
    // request would otherwise pay that wait)
    const double tnow = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t freed = 0;
    if (runRelief(2.0, freed))
    {
        const bool ok = fits(usage, budget);
        char buf[320];
        std::snprintf(buf, sizeof(buf), "%s: %llu MB over the heap budget (%llu/%llu MB used); relief freed %llu MB -> %s",
                      what, (unsigned long long)(bytes >> 20), (unsigned long long)(usage >> 20),
                      (unsigned long long)(budget >> 20), (unsigned long long)(freed >> 20), ok ? "fits" : "still over");
        allocLog(buf);
        if (ok) return true;
    }
    if (soft)
    {
        softFail(VK_ERROR_OUT_OF_DEVICE_MEMORY, what, bytes);
        return false;
    }
    if (tnow - mLastHardOverLog >= 10.0)
    {
        mLastHardOverLog = tnow;
        char buf[320];
        std::snprintf(buf, sizeof(buf), "%s: HARD allocation of %llu MB over the heap budget (%llu/%llu MB used) -- "
                                        "proceeding (no soft path for it)",
                      what, (unsigned long long)(bytes >> 20), (unsigned long long)(usage >> 20),
                      (unsigned long long)(budget >> 20));
        allocLog(buf);
    }
    return true;
}

// ---- #695 UMA: integrated GPUs with a BIOS carve-out ----
// AMD APUs (Ryzen 5700G / Vega 8, Windows and RADV) report the carve-out -- 512 MB by
// default on AM4 -- as the DEVICE_LOCAL heap and the shared system memory as a large host
// heap. Both are the same DRAM behind the same memory controller. Budgeting the carve-out
// alone gave a ~240 MB VRAM budget, and the pressure ladder sat deep in its rungs forever
// (blurry textures, 1/4 mesh detail, probe count 1/2, short draw distance).
// UMA mode: an INTEGRATED_GPU whose largest non-device-local heap is LARGER than its
// largest device-local heap (a carve-out layout; Intel / ANV report one big DEVICE_LOCAL
// heap and stay on the old path), or any device with RenderVKDebugForceUma.
// RenderVKUmaSharedRamPercent = 0 turns it off.
void VkDeviceContext::detectUma(VkPhysicalDeviceType type)
{
    mDeviceType = type;
    mUma = mUmaForced = false;
    mUmaDevHeap = mUmaHostHeap = ~0u;
    mUmaHostCeil = mUmaFakeCarve = 0;
    const VkUmaConfig cfg = gVkUmaConfig;
    uint32_t dev = ~0u, host = ~0u;
    for (uint32_t h = 0; h < mMemProps.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
    {
        const bool dl = (mMemProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        uint32_t& best = dl ? dev : host;
        if (best == ~0u || mMemProps.memoryHeaps[h].size > mMemProps.memoryHeaps[best].size) best = h;
    }
    if (dev == ~0u || host == ~0u || cfg.sharedRamPercent == 0) return;
    const bool carveLayout = type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                             && mMemProps.memoryHeaps[host].size > mMemProps.memoryHeaps[dev].size;
    if (!carveLayout && !cfg.forceUma) return;
    mUma = true;
    mUmaForced = !carveLayout;
    mUmaDevHeap = dev;
    mUmaHostHeap = host;
    // The host-heap CEILING: a share of physical RAM (default 40%), never more than the
    // host heap. The rest stays with the CPU side: the system governor's reserve for other
    // programs (LLMemoryGovernor, max(1.5 GB, 12% of RAM)) plus the viewer's own working
    // set (decoded copies, caches, the in-flight decode budget). Physical RAM, not the
    // governor's live "available": GPU memory is RETAINED, and a ceiling derived from
    // what is free right now would shrink as our own textures fill it (and oscillate).
    // The governor still sees the GPU's host-heap pages as used RAM, so a machine that
    // runs short goes TIGHT and trims the CPU side -- the same RAM is counted once
    // per side, never twice. Without a RAM figure: half the host heap.
    const uint32_t pct = std::min<uint32_t>(cfg.sharedRamPercent, 90u);
    const uint64_t hostSize = mMemProps.memoryHeaps[host].size;
    mUmaHostCeil = cfg.ramTotal ? std::min<uint64_t>(cfg.ramTotal / 100u * pct, hostSize) : hostSize / 2u;
    mUmaFakeCarve = (uint64_t)cfg.fakeCarveoutMB << 20;
}

bool VkDeviceContext::inBudgetHeap(uint32_t heap) const
{
    if (heap >= mMemProps.memoryHeapCount) return false;
    if (mUma) return heap == mUmaDevHeap || heap == mUmaHostHeap;
    return (mMemProps.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
}

uint32_t VkDeviceContext::umaHostType(uint32_t typeBits, VkMemoryPropertyFlags props) const
{
    if (!mUma) return ~0u;
    const VkMemoryPropertyFlags want = props & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    // (0x40 / 0x80: VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD / DEVICE_UNCACHED_BIT_AMD --
    // debugging types, slow; never picked)
    const VkMemoryPropertyFlags never = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT
                                      | (VkMemoryPropertyFlags)0x40u | (VkMemoryPropertyFlags)0x80u;
    uint32_t best = ~0u;
    int bestScore = -1;
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i)
    {
        const VkMemoryType& t = mMemProps.memoryTypes[i];
        if (!(typeBits & (1u << i)) || t.heapIndex != mUmaHostHeap) continue;
        if ((t.propertyFlags & want) != want || (t.propertyFlags & never)) continue;
        // Uncached (write-combined) first: GPU reads of cached host memory are snooped
        // (AMD), unless the caller asked for HOST_CACHED.
        int score = 2;
        if ((t.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) && !(want & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) score = 1;
        if (score > bestScore) { best = i; bestScore = score; }
    }
    return best;
}

bool VkDeviceContext::umaCarveFits(uint64_t bytes)
{
    if (!mUma || mUmaDevHeap >= VK_MAX_MEMORY_HEAPS) return true;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint64_t tracked = 0;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        tracked = mHeapUsed[mUmaDevHeap];
    }
    uint64_t usage = 0, budget = 0;
    {
        std::lock_guard<std::mutex> ul(mUmaMutex.m);
        // The driver's numbers at most every 250 ms (the query is a kernel call on Windows,
        // and every device-local allocation asks); our own allocations since are added.
        if (now - mUmaSnapAt >= 0.25)
        {
            mUmaSnapAt = now;
            mUmaSnapTracked = tracked;
            mUmaSnapBudget = mMemProps.memoryHeaps[mUmaDevHeap].size;
            mUmaSnapUsage = tracked;
            if (mMemBudgetExt && mPhysical)
            {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
                VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
                p2.pNext = &bud;
                vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
                if (bud.heapBudget[mUmaDevHeap]) mUmaSnapBudget = bud.heapBudget[mUmaDevHeap];
                mUmaSnapUsage = std::max<uint64_t>(bud.heapUsage[mUmaDevHeap], tracked);
            }
            if (mUmaFakeCarve) mUmaSnapBudget = std::min(mUmaSnapBudget, mUmaFakeCarve);
        }
        budget = mUmaSnapBudget;
        usage = std::max(tracked, mUmaSnapUsage + (tracked > mUmaSnapTracked ? tracked - mUmaSnapTracked : 0));
    }
    // The pre-flight's margins (heapBudgetPreflight): >= 64 MB keep ~3% (at least 256 MB)
    // free -- a big request goes to the host heap rather than pass the carve-out's wall --
    // smaller ones 1.5%.
    const uint64_t margin = bytes >= (64ull << 20) ? std::max<uint64_t>(budget / 32u, 256ull << 20) : budget / 64u;
    return usage + bytes + margin <= budget;
}

uint32_t VkDeviceContext::pickDeviceLocalType(uint32_t typeBits, VkMemoryPropertyFlags props, uint64_t bytes)
{
    const uint32_t type = findMemoryType(typeBits, props);   // (the old pick; aborts on no match)
    if (!mUma || mMemProps.memoryTypes[type].heapIndex != mUmaDevHeap || umaCarveFits(bytes)) return type;
    // The carve-out is full: the same DRAM through the host heap (spill). No usable
    // host type for this resource (an image the driver keeps in device memory): as before.
    const uint32_t host = umaHostType(typeBits, props);
    if (host == ~0u) return type;
    std::lock_guard<std::mutex> ul(mUmaMutex.m);
    ++mUmaSpills;
    mUmaSpillBytes += bytes;
    return host;
}

VkResult VkDeviceContext::allocateMemoryUma(VkMemoryAllocateInfo& ai, VkDeviceMemory* mem, const char* what,
                                            uint32_t typeBits, VkMemoryPropertyFlags props)
{
    const VkResult r = allocateMemory(ai, mem, what);
    if (!mUma || !(props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        || (r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_ERROR_OUT_OF_HOST_MEMORY)
        || ai.memoryTypeIndex >= mMemProps.memoryTypeCount
        || mMemProps.memoryTypes[ai.memoryTypeIndex].heapIndex != mUmaDevHeap)
        return r;
    // The carve-out refused (the driver's budget moved below our snapshot): the host heap.
    const uint32_t host = umaHostType(typeBits, props);
    if (host == ~0u) return r;
    const uint32_t prev = ai.memoryTypeIndex;
    ai.memoryTypeIndex = host;
    const VkResult r2 = allocateMemory(ai, mem, what);
    if (r2 != VK_SUCCESS)
    {
        ai.memoryTypeIndex = prev;
        return r;
    }
    std::lock_guard<std::mutex> ul(mUmaMutex.m);
    ++mUmaSpills;
    mUmaSpillBytes += ai.allocationSize;
    return r2;
}

VkDeviceContext::BudgetHeaps VkDeviceContext::budgetHeaps(const MemStats& m)
{
    BudgetHeaps b;
    if (m.uma && m.umaDevHeap < VK_MAX_MEMORY_HEAPS && m.umaHostHeap < VK_MAX_MEMORY_HEAPS)
    {
        // Carve-out budget (clamped by RenderVKDebugFakeCarveoutMB in memStats) + the host
        // heap's budget up to the RAM-share ceiling (detectUma).
        b.uma = true;
        b.heap = m.umaDevHeap;
        b.hostHeap = m.umaHostHeap;
        b.devBudget = m.heapBudget[m.umaDevHeap];
        b.hostBudget = std::min(m.heapBudget[m.umaHostHeap], m.umaHostCeil);
        b.budget = b.devBudget + b.hostBudget;
        return b;
    }
    // Not UMA: the device-local heap with the largest budget (unchanged rule).
    for (uint32_t h = 0; h < m.heapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
    {
        if (!m.heapDeviceLocal[h] || m.heapBudget[h] == 0 || m.heapBudget[h] <= b.budget) continue;
        b.budget = m.heapBudget[h];
        b.heap = h;
    }
    return b;
}

std::string VkDeviceContext::memLayoutText() const
{
    static const char* kType[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    const MemStats m = memStats();
    const BudgetHeaps bh = budgetHeaps(m);
    std::string o = "device ";
    o += (uint32_t)mDeviceType < 5u ? kType[(uint32_t)mDeviceType] : "?";
    o += m.budgetExt ? ", heaps (size/budget/usage MB):" : ", heaps (size MB; no VK_EXT_memory_budget):";
    char buf[160];
    for (uint32_t h = 0; h < m.heapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
    {
        const VkMemoryHeap& hp = mMemProps.memoryHeaps[h];
        std::string types;
        for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i)
            if (mMemProps.memoryTypes[i].heapIndex == h)
            {
                std::snprintf(buf, sizeof(buf), "%s%u:0x%x", types.empty() ? "" : ",", i,
                              (unsigned)mMemProps.memoryTypes[i].propertyFlags);
                types += buf;
            }
        if (m.budgetExt)
            std::snprintf(buf, sizeof(buf), " [%u %s %llu/%llu/%llu types %s]", h,
                          (hp.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DL" : "host",
                          (unsigned long long)(hp.size >> 20), (unsigned long long)(m.heapBudget[h] >> 20),
                          (unsigned long long)(m.heapDriverUsage[h] >> 20), types.c_str());
        else
            std::snprintf(buf, sizeof(buf), " [%u %s %llu types %s]", h,
                          (hp.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DL" : "host",
                          (unsigned long long)(hp.size >> 20), types.c_str());
        o += buf;
    }
    if (bh.uma)
    {
        std::snprintf(buf, sizeof(buf), "; budget mode UMA%s (#695): carve-out heap %u %llu MB%s + host heap %u %llu MB "
                                        "(ceiling %llu MB) = %llu MB",
                      mUmaForced ? " (FORCED, RenderVKDebugForceUma)" : "", bh.heap,
                      (unsigned long long)(bh.devBudget >> 20), mUmaFakeCarve ? " (fake carve-out)" : "", bh.hostHeap,
                      (unsigned long long)(bh.hostBudget >> 20), (unsigned long long)(mUmaHostCeil >> 20),
                      (unsigned long long)(bh.budget >> 20));
        o += buf;
    }
    else if (bh.heap != ~0u)
    {
        std::snprintf(buf, sizeof(buf), "; budget mode device-local heap %u (%llu MB)", bh.heap,
                      (unsigned long long)(bh.budget >> 20));
        o += buf;
    }
    else
        o += "; budget mode: no device-local heap";
    return o;
}

uint32_t VkDeviceContext::findMemoryType(uint32_t typeBits,
                                         VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mMemProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    std::fprintf(stderr, "no suitable memory type\n"); std::abort();
    return 0;
}

// Async compute with a dedicated compute family (phase 3a): every buffer and every image
// the async queue could touch is CONCURRENT across {graphics, compute}, so neither queue
// needs a queue-family ownership transfer. Buffers have no compression to lose. Images:
// CONCURRENT may disable a driver's framebuffer / depth compression on some hardware
// (AMD DCC / HTILE especially), so pure attachments that no compute pass can reach (no
// SAMPLED / STORAGE / TRANSFER usage) stay EXCLUSIVE. If profiling shows the cost on the
// 4K depth / HDR targets, move those to EXCLUSIVE + release / acquire barriers around
// the compute segments that read them.
void VkDeviceContext::applySharing(VkSharingMode& mode, uint32_t& count, const uint32_t*& families,
                                   bool image, VkImageUsageFlags usage) const
{
    if (!mConcurrent) return;
    if (image && !(usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return;
    mode = VK_SHARING_MODE_CONCURRENT;
    count = 2;
    families = mShareFamilies;
}

GpuBuffer VkDeviceContext::createBuffer(VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags props,
                                        bool deviceAddress, const char* site) {
    GpuBuffer b{};
    b.size = size;
    // Every buffer gets a device address (bufferDeviceAddress is always enabled) so the
    // fault registry can map it; b.address is still set only when the caller asked.
    usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, false, 0);
    {
        const VkResult r = vkCreateBuffer(mDevice, &ci, nullptr, &b.buffer);
        if (r != VK_SUCCESS && softFail(r, "createBuffer", size)) return GpuBuffer{};
        VK_CHECK(r);
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mDevice, b.buffer, &req);

    VkMemoryAllocateFlagsInfo flags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    // HOST_CACHED is a PREFERENCE (readback buffers): CPU reads from uncached
    // (write-combined) host-visible memory stall on every load -- a per-frame GPU
    // readback cost ~30% of the main thread. Fall back to the uncached type when the
    // device has no cached one for this buffer.
    if (props & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
    {
        uint32_t idx = ~0u;
        for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mMemProps.memoryTypes[i].propertyFlags & props) == props)
                { idx = i; break; }
        ai.memoryTypeIndex = idx != ~0u ? idx
            : findMemoryType(req.memoryTypeBits, props & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    else if (props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ai.memoryTypeIndex = pickDeviceLocalType(req.memoryTypeBits, props, req.size);   // #695 (else findMemoryType)
    else
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    ai.pNext = &flags;
    // Pre-flight a large allocation against the live heap budget (#563, heapBudgetPreflight):
    // NVIDIA segfaults inside vkAllocateMemory when a big request pushes the heap past its
    // budget instead of returning OUT_OF_DEVICE_MEMORY (two crashes, 2026-10-08).
    if (!heapBudgetPreflight(ai.memoryTypeIndex, req.size, "createBuffer (heap budget pre-flight)"))
    {
        vkDestroyBuffer(mDevice, b.buffer, nullptr);
        return GpuBuffer{};
    }
    {
        const VkResult r = (props & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? allocateMemory(ai, &b.memory, "createBuffer")
            : allocateMemoryUma(ai, &b.memory, "createBuffer", req.memoryTypeBits, props);   // #695
        if (r != VK_SUCCESS && softFail(r, "createBuffer", req.size)) {
            vkDestroyBuffer(mDevice, b.buffer, nullptr);
            return GpuBuffer{};
        }
        VK_CHECK(r);
    }
    trackAlloc(b.memory, req.size, ai.memoryTypeIndex, false, (uint32_t)usage,
               (usage & (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                         | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT)) != 0, site);
    {   // #697: soft mode -- out of memory binding frees the buffer + its memory
        const VkResult r = vkBindBufferMemory(mDevice, b.buffer, b.memory, 0);
        if (r != VK_SUCCESS && softFail(r, "createBuffer(bind)", req.size)) { destroyBuffer(b); return GpuBuffer{}; }
        VK_CHECK(r);
    }

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        const VkResult r = vkMapMemory(mDevice, b.memory, 0, size, 0, &b.mapped);
        if (r != VK_SUCCESS && softFail(r, "createBuffer(map)", size)) {
            b.mapped = nullptr;
            destroyBuffer(b);
            return GpuBuffer{};
        }
        VK_CHECK(r);
    }
    {
        VkBufferDeviceAddressInfo di{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        di.buffer = b.buffer;
        const VkDeviceAddress a = vkGetBufferDeviceAddress(mDevice, &di);
        if (deviceAddress) b.address = a;
        vkAddrTrackBuffer(b.buffer, a, req.size, (uint32_t)usage, __builtin_return_address(0));
    }
    static std::atomic<uint64_t> s_serial{0};
    b.serial = ++s_serial;
    return b;
}

// Lost device (#135): object teardown leaks to process exit instead of entering the
// driver (destroying is legal on a lost device, but some drivers crash in vkDestroy* /
// vkFreeMemory after a TDR, and the process is about to exit anyway).
void VkDeviceContext::destroyBuffer(GpuBuffer& b) {
    if (vkDeviceIsLost()) { b = GpuBuffer{}; return; }
    if (b.mapped) { vkUnmapMemory(mDevice, b.memory); b.mapped = nullptr; }
    if (b.buffer) { vkAddrUntrackBuffer(b.buffer); vkDestroyBuffer(mDevice, b.buffer, nullptr); }
    if (b.memory) { trackFree(b.memory); vkFreeMemory(mDevice, b.memory, nullptr); }
    b = GpuBuffer{};
}

// ---- image memory VA (image registry; vk_common.h vkImgTrack) ----
// Images use dedicated allocations (no VkMemoryDedicatedAllocateInfo), so a throwaway
// buffer may alias the allocation at offset 0: its device address is the memory's GPU
// VA, which is what a fault on the image reports. The allocation then needs
// DEVICE_ADDRESS (bufferDeviceAddress is always enabled: every buffer uses it).
// SLIPSTREAM_VK_NO_IMAGE_VA=1: no flag, no probe (images allocated exactly as before).
static bool imageVaWanted()
{
    static int s_on = -1;
    if (s_on < 0)
    {
        const char* e = sl_getenv("VK_NO_IMAGE_VA");
        s_on = (e && *e && *e != '0') ? 0 : 1;
    }
    return s_on == 1;
}

static VkDeviceAddress probeMemoryVA(VkDevice dev, VkDeviceMemory mem, uint32_t memType, VkDeviceSize allocSize)
{
    if (!imageVaWanted() || !mem) return 0;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 256;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bi, nullptr, &b) != VK_SUCCESS) return 0;
    VkMemoryRequirements r{};
    vkGetBufferMemoryRequirements(dev, b, &r);
    VkDeviceAddress a = 0;
    if ((r.memoryTypeBits & (1u << memType)) && r.size <= allocSize && vkBindBufferMemory(dev, b, mem, 0) == VK_SUCCESS)
    {
        VkBufferDeviceAddressInfo di{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        di.buffer = b;
        a = vkGetBufferDeviceAddress(dev, &di);
    }
    vkDestroyBuffer(dev, b, nullptr);
    return a;
}

// ---------------------------------------------------------------------------
// TEXTURE SUBALLOCATION (#411): VMA pools for texture + UI images
// ---------------------------------------------------------------------------
// Every vkAllocateMemory is rounded up to the driver's granularity (64 KiB on NVIDIA,
// measured at device creation, #407), so one allocation per small texture wasted ~1 GB
// in a busy region. Texture / UI images whose memory requirement fits are suballocated:
//   * SMALL (<= the measured granularity): size-class SLABS, one VMA pool per class
//     (2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64 KiB; the series continues x1.5 / x2 on a
//     coarser driver). Every allocation in a class pool is the class size, so any free
//     slot fits the next image of the class: no external fragmentation, internal
//     rounding <= 1/3. An image whose alignment doesn't divide a class skips that class.
//   * MEDIUM (up to RenderVKTexturePoolMediumMaxKB): one TLSF pool.
//   * everything else -- larger images, anything with attachment / storage usage, images
//     the driver prefers dedicated -- keeps its own allocation, as before.
// Allocation uses VMA's MIN_MEMORY strategy: blocks are tried fullest first (VMA keeps
// them approximately sorted by free space, one bubble swap per operation), so the images
// re-created on every mip change drain sparse blocks over time. Pools have no minimum
// block count; VMA frees an empty block on the pool's next free that does not empty
// another (a one-block hysteresis per pool -- VMA has no way to turn that off).
// Block sizes are small on purpose (4 / 8 MiB by default): a block is only released when
// EVERY image in it is gone, and scattered frees (unloads, ladder mip-downs) strand
// partially used blocks -- smaller blocks strand less (see third_party/vma/README.md).
namespace {
void VKAPI_PTR vmaOnBlockAlloc(VmaAllocator, uint32_t memoryType, VkDeviceMemory memory, VkDeviceSize size,
                               void* user)
{
    if (user) static_cast<VkDeviceContext*>(user)->onVmaBlock(true, memoryType, memory, (uint64_t)size);
}
void VKAPI_PTR vmaOnBlockFree(VmaAllocator, uint32_t memoryType, VkDeviceMemory memory, VkDeviceSize size,
                              void* user)
{
    if (user) static_cast<VkDeviceContext*>(user)->onVmaBlock(false, memoryType, memory, (uint64_t)size);
}
constexpr VkImageUsageFlags kPoolableUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
uint64_t roundUpMiB(uint64_t b) { return (b + (1ull << 20) - 1) & ~((1ull << 20) - 1); }
} // namespace

void VkDeviceContext::initTexturePools()
{
    mTexPoolStatus = "off";
    const VkTexPoolConfig cfg = gVkTexPoolConfig;
    if (!cfg.enabled) { mTexPoolStatus = "off (RenderVKTextureSuballoc = 0)"; VK_LOG("texture pools: %s", mTexPoolStatus.c_str()); return; }

    // The memory type: the one the dedicated path picks for a texture (first device-local
    // type the image accepts); each allocation re-checks its own memoryTypeBits.
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {64, 64, 1};
    ci.mipLevels = 7;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = kPoolableUsage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, ci.usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceImageMemoryRequirements di{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
    di.pCreateInfo = &ci;
    VkMemoryRequirements2 r2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(mDevice, &di, &r2);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount && type == ~0u; ++i)
        if ((r2.memoryRequirements.memoryTypeBits & (1u << i))
            && (mMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == ~0u) { mTexPoolStatus = "unavailable (no device-local type for textures)"; VK_LOG("texture pools: %s", mTexPoolStatus.c_str()); return; }
    // #695 UMA: the pools live on the LARGER part of the budget. A default BIOS carve-out
    // (512 MB) is smaller than the host-heap share, so textures go to the host heap (the
    // same DRAM; write-combined) and the carve-out is left to render targets, buffers and
    // dedicated images; a large carve-out keeps them device-local (and spills -- the
    // dedicated path, pickDeviceLocalType -- once it is full). Only when the driver lets
    // optimal-tiled textures live in a host-heap type.
    if (mUma)
    {
        uint64_t devB = mMemProps.memoryHeaps[mUmaDevHeap].size, hostB = mMemProps.memoryHeaps[mUmaHostHeap].size;
        if (mMemBudgetExt)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
            p2.pNext = &bud;
            vkGetPhysicalDeviceMemoryProperties2(mPhysical, &p2);
            if (bud.heapBudget[mUmaDevHeap]) devB = bud.heapBudget[mUmaDevHeap];
            if (bud.heapBudget[mUmaHostHeap]) hostB = bud.heapBudget[mUmaHostHeap];
        }
        if (mUmaFakeCarve) devB = std::min(devB, mUmaFakeCarve);
        hostB = std::min(hostB, mUmaHostCeil);
        const uint32_t ht = umaHostType(r2.memoryRequirements.memoryTypeBits, 0);
        if (hostB > devB && ht != ~0u) type = ht;
        VK_LOG("texture pools (#695 UMA): carve-out %llu MB, host share %llu MB -> memory type %u (%s)%s",
               (unsigned long long)(devB >> 20), (unsigned long long)(hostB >> 20), type,
               type == ht ? "host heap" : "carve-out",
               ht == ~0u ? "; the driver allows no host-heap type for textures" : "");
    }

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaDeviceMemoryCallbacks cb{};
    cb.pfnAllocate = vmaOnBlockAlloc;
    cb.pfnFree = vmaOnBlockFree;
    cb.pUserData = this;
    VmaAllocatorCreateInfo ai{};
    // bufferDeviceAddress is always enabled (createLogicalDevice): blocks get
    // DEVICE_ADDRESS, so the image registry can resolve a fault address to a pooled image.
    ai.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (mMemBudgetExt) ai.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    ai.physicalDevice = mPhysical;
    ai.device = mDevice;
    ai.instance = mInstance;
    ai.vulkanApiVersion = mApi12 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_3;   // the device's chosen version (#675)
    if (mApi12 && mMaint4) ai.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    ai.pVulkanFunctions = &vf;
    ai.pDeviceMemoryCallbacks = &cb;
    if (vmaCreateAllocator(&ai, &mVma) != VK_SUCCESS)
    {
        mVma = nullptr;
        mTexPoolStatus = "unavailable (vmaCreateAllocator failed)";
        VK_LOG("texture pools: %s", mTexPoolStatus.c_str());
        return;
    }

    // Size classes: the small tier ends at the driver's allocation granularity (what a
    // dedicated allocation would round up to); a coarser driver continues the series.
    mTexSmallMax = mAllocGran;
    mTexMediumMax = (uint64_t)std::min<uint32_t>(cfg.mediumMaxKB, 64u * 1024u) << 10;
    std::vector<uint32_t> classes;
    static const uint32_t kClassKiB[] = {2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
    for (uint32_t kb : kClassKiB)
        if (((uint64_t)kb << 10) <= mTexSmallMax) classes.push_back(kb << 10);
    for (uint64_t p = 64ull << 10; classes.size() + 1 < MemStats::kTexPoolMax; p <<= 1)
    {
        if (p * 3 / 2 > mTexSmallMax) break;
        classes.push_back((uint32_t)(p * 3 / 2));
        if (p * 2 > mTexSmallMax) break;
        classes.push_back((uint32_t)(p * 2));
    }
    // MEDIUM SIZE CLASSES (#587): one slab pool per class above the small tier, so a freed
    // slot is always reusable by the next image of its class (the single TLSF medium pool
    // stranded ~25% of its blocks in mixed-size holes). SL textures are power-of-two with
    // full mip chains, so their memory requirements cluster (NVIDIA, measured 2026-10-09:
    // 87 / 173 / 343 / 685 / 1367 / 2733 KiB, each cluster spread over a few KiB by aspect
    // ratio and format); the exact sizes are the driver's tiling / mip-tail padding, so
    // they are PROBED here with vkGetDeviceImageMemoryRequirements on the shapes the
    // texture path creates (same usage / flags / format list as createImage2D), not
    // assumed. Probing is deterministic per driver and needs no persisted state; mTexPools
    // stays immutable after init (read lock-free by allocation and stats).
    std::vector<uint32_t> medClasses;
    uint64_t medAlign = 1;   // largest alignment of a probed medium image
    {
        // Probes: (size, alignment, common shape?) of the canonical texture images whose
        // memory requirement lands in the medium range.
        struct Probe { uint64_t size, align; bool common; };
        std::vector<Probe> probes;
        auto probe = [&](VkFormat f, VkFormat alt, uint32_t w, uint32_t h, bool common)
        {
            VkImageCreateInfo pci = ci;
            const VkFormat pfl[2] = {f, alt};
            VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
            pci.pNext = nullptr;
            pci.flags = 0;
            if (alt != VK_FORMAT_UNDEFINED && alt != f)
            {
                pci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
                fl.viewFormatCount = 2;
                fl.pViewFormats = pfl;
                pci.pNext = &fl;
            }
            pci.format = f;
            pci.extent = {w, h, 1};
            pci.mipLevels = 32u - (uint32_t)__builtin_clz(std::max(w, h));   // full chain (texMipLevels)
            VkDeviceImageMemoryRequirements pdi{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
            pdi.pCreateInfo = &pci;
            VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 pr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            pr.pNext = &dr;
            vkGetDeviceImageMemoryRequirements(mDevice, &pdi, &pr);
            const VkMemoryRequirements& q = pr.memoryRequirements;
            if (dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation) return;
            if (!(q.memoryTypeBits & (1u << type))) return;
            if (q.size <= mTexSmallMax || q.size > mTexMediumMax) return;
            probes.push_back(Probe{(uint64_t)q.size, std::max<uint64_t>(q.alignment, 1), common});
        };
        struct Fmt { VkFormat f, alt; };
        std::vector<Fmt> fmts = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
                                 {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
                                 {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED}};   // UI / non-mutable
        if (mTexCompBC)
        {
            fmts.push_back({VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK});
            fmts.push_back({VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK});
            fmts.push_back({VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK});
            fmts.push_back({VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_UNDEFINED});
            fmts.push_back({VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_UNDEFINED});
        }
        // Power-of-two shapes 4..4096, aspect up to 8:1 (rarer shapes go to the fallback).
        for (const Fmt& fm : fmts)
            for (uint32_t lw = 2; lw <= 12; ++lw)
                for (uint32_t lh = 2; lh <= 12; ++lh)
                {
                    const uint32_t d = lw > lh ? lw - lh : lh - lw;
                    if (d > 3) continue;
                    probe(fm.f, fm.alt, 1u << lw, 1u << lh, d <= 1);
                }
        for (const Probe& p : probes) medAlign = std::max(medAlign, p.align);

        // Configured classes (RenderVKTexturePoolClassesKB).
        bool wantAuto = false, wantDefault = false, wantNone = false;
        std::vector<uint64_t> listed;
        {
            const std::string& s = cfg.classesKB;
            auto sep = [](char c) { return c == ',' || c == ' ' || c == ';' || c == '\t'; };
            size_t i = 0;
            while (i < s.size())
            {
                while (i < s.size() && sep(s[i])) ++i;
                size_t j = i;
                while (j < s.size() && !sep(s[j])) ++j;
                if (j > i)
                {
                    std::string tok = s.substr(i, j - i);
                    for (char& c : tok)
                        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                    if (tok == "auto") wantAuto = true;
                    else if (tok == "default") wantDefault = true;
                    else if (tok == "none") wantNone = true;
                    else
                    {
                        char* end = nullptr;
                        const unsigned long kb = std::strtoul(tok.c_str(), &end, 10);
                        if (end && *end == '\0' && kb > 0 && kb <= 64ul * 1024ul) listed.push_back((uint64_t)kb << 10);
                        else VK_LOG("texture pools: RenderVKTexturePoolClassesKB: ignoring '%s'", tok.c_str());
                    }
                }
                i = j;
            }
            if (!wantDefault && !wantNone && listed.empty()) wantAuto = true;   // "" = auto
        }
        // Built-in list (NVIDIA, measured cluster maxima + ~1.5% headroom): used for
        // "default", and for "auto" when the probe found nothing in the medium range.
        static const uint32_t kDefaultMedKiB[] = {90, 177, 352, 698, 1392, 2780};
        if (wantAuto && probes.empty()) wantDefault = true;

        // Every class is a multiple of at least 4 KiB (and of the largest probed alignment
        // for the default / listed ones), so images whose alignment the probe did not see
        // (up to 4 KiB) still divide the slot size (texPoolFor checks it).
        constexpr uint64_t kSlotAlign = 4096;
        const uint64_t slotAlign = std::max<uint64_t>(medAlign, kSlotAlign);
        struct Cand { uint64_t bytes; uint32_t common; };
        std::vector<Cand> cands;
        const auto alignUp = [](uint64_t v, uint64_t a) { return (v + a - 1) / a * a; };
        if (wantAuto && !probes.empty())
        {
            // Cluster: sizes within 1/16 of the cluster's smallest share a class; the class
            // is the cluster MAX + 1/64 headroom (unprobed variants a few KiB larger),
            // rounded up to the cluster's alignment. Per-image rounding waste <= ~8%.
            std::sort(probes.begin(), probes.end(), [](const Probe& a, const Probe& b) { return a.size < b.size; });
            size_t i = 0;
            while (i < probes.size())
            {
                const uint64_t lo = probes[i].size;
                uint64_t hiAll = lo, hiCommon = 0, al = 1;
                uint32_t common = 0;
                size_t j = i;
                for (; j < probes.size() && probes[j].size <= lo + lo / 16; ++j)
                {
                    hiAll = std::max(hiAll, probes[j].size);
                    al = std::max(al, probes[j].align);
                    if (probes[j].common) { ++common; hiCommon = std::max(hiCommon, probes[j].size); }
                }
                // Sized for the common (<= 2:1) shapes when the cluster has any: a 4:1 / 8:1
                // shape a few % larger would otherwise inflate every slot of the class (it
                // still fits with <= 1/8 waste, or goes to the fallback).
                const uint64_t hi = common ? hiCommon : hiAll;
                cands.push_back(Cand{alignUp(hi + hi / 64, std::max<uint64_t>(al, kSlotAlign)), common});
                i = j;
            }
        }
        if (wantDefault)
            for (uint32_t kb : kDefaultMedKiB) cands.push_back(Cand{alignUp((uint64_t)kb << 10, slotAlign), 1});
        for (uint64_t b : listed) cands.push_back(Cand{alignUp(b, slotAlign), ~0u});   // listed: always kept
        // Range, de-duplicate (keep the strongest priority), bound the count: the small
        // classes + the medium classes + the fallback pool fit kTexPoolMax; when they
        // don't, the clusters with the fewest common (<= 2:1) shapes are dropped first.
        cands.erase(std::remove_if(cands.begin(), cands.end(),
                                   [&](const Cand& c) { return c.bytes <= mTexSmallMax || c.bytes > mTexMediumMax
                                                               || c.bytes > 0xffffffffull; }),
                    cands.end());
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b)
                  { return a.bytes != b.bytes ? a.bytes < b.bytes : a.common > b.common; });
        cands.erase(std::unique(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.bytes == b.bytes; }),
                    cands.end());
        const size_t room = MemStats::kTexPoolMax > classes.size() + 1 ? MemStats::kTexPoolMax - classes.size() - 1 : 0;
        if (cands.size() > room)
        {
            std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.common > b.common; });
            cands.resize(room);
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.bytes < b.bytes; });
        }
        for (const Cand& c : cands) medClasses.push_back((uint32_t)c.bytes);
        VK_LOG("texture pools: medium classes (%s%s%s%s): %zu probed sizes, alignment %llu",
               wantAuto ? "auto" : "", wantDefault ? " default" : "", listed.empty() ? "" : " listed",
               wantNone ? " none" : "", probes.size(), (unsigned long long)medAlign);
    }
    const uint64_t smallBlock = (uint64_t)std::max<uint32_t>(cfg.smallBlockMB, 1u) << 20;
    auto makePool = [&](uint32_t classBytes, uint64_t blockSize) -> bool
    {
        if (mTexPools.size() >= MemStats::kTexPoolMax) return false;   // mTexPoolLive[] / stats bound
        VmaPoolCreateInfo pc{};
        pc.memoryTypeIndex = type;
        pc.flags = VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT;   // optimal-tiled images only
        pc.blockSize = blockSize;
        pc.minBlockCount = 0;
        pc.maxBlockCount = 0;
        VmaPool pool = nullptr;
        if (vmaCreatePool(mVma, &pc, &pool) != VK_SUCCESS) return false;
        char name[48];
        if (classBytes) std::snprintf(name, sizeof(name), "tex slab %u KiB", (classBytes + 1023) >> 10);
        else std::snprintf(name, sizeof(name), "tex medium");
        vmaSetPoolName(mVma, pool, name);
        mTexPools.push_back(TexPoolDef{pool, classBytes, blockSize, classBytes ? (uint32_t)(blockSize / classBytes) : 0u});
        return true;
    };
    bool ok = true;
    for (uint32_t c : classes)
        ok = ok && makePool(c, std::max(smallBlock, roundUpMiB((uint64_t)c * 16)));
    // Medium slabs: 16 slots per block while that stays <= 16 MiB, else 8 (8..24 MiB blocks
    // for the big classes). A block frees only when all its slots are empty, so fewer
    // slots strand less after an unload; more slots mean fewer vkAllocateMemory calls.
    // Rounded up to the granularity (the tail is < one slot: a class exceeds it).
    const uint64_t gran = mAllocGran ? mAllocGran : 1;
    std::string medText;
    for (uint32_t c : medClasses)
    {
        const uint64_t slots = (uint64_t)c * 16 <= (16ull << 20) ? 16 : 8;
        const uint64_t blk = ((uint64_t)c * slots + gran - 1) / gran * gran;
        ok = ok && makePool(c, blk);
        char t[32];
        std::snprintf(t, sizeof(t), "%s%u", medText.empty() ? "" : " ", (c + 1023) >> 10);
        medText += t;
    }
    const bool medium = mTexMediumMax > mTexSmallMax;
    const uint64_t mediumBlock = medium
        ? std::max((uint64_t)std::max<uint32_t>(cfg.mediumBlockMB, 1u) << 20, roundUpMiB(mTexMediumMax * 2)) : 0;
    if (medium) ok = ok && makePool(0, mediumBlock);
    if (!ok || mTexPools.empty())
    {
        destroyTexturePools();
        mTexPoolStatus = ok ? "off (no class fits the granularity and RenderVKTexturePoolMediumMaxKB)"
                            : "unavailable (vmaCreatePool failed)";
        VK_LOG("texture pools: %s", mTexPoolStatus.c_str());
        return;
    }
    mTexPoolType = type;
    mTexPoolDev = inBudgetHeap(mMemProps.memoryTypes[type].heapIndex);   // (#695: UMA host-heap pools count too)
    char buf[256];
    if (!classes.empty())
        std::snprintf(buf, sizeof(buf), "on: %u slab classes %u-%u KiB (%llu MiB blocks)", (uint32_t)classes.size(),
                      classes.front() >> 10, classes.back() >> 10, (unsigned long long)(smallBlock >> 20));
    else
        std::snprintf(buf, sizeof(buf), "on: no slab classes (granularity %llu KiB)", (unsigned long long)(mAllocGran >> 10));
    mTexPoolStatus = buf;
    if (!medClasses.empty())
    {
        std::snprintf(buf, sizeof(buf), " (16 slots/block, 8 above 1 MiB; probed alignment %llu)",
                      (unsigned long long)medAlign);
        mTexPoolStatus += ", medium classes KiB [" + medText + "]" + buf;
    }
    if (medium)
    {
        std::snprintf(buf, sizeof(buf), ", fallback TLSF <= %llu KiB (%llu MiB blocks)", (unsigned long long)(mTexMediumMax >> 10),
                      (unsigned long long)(mediumBlock >> 20));
        mTexPoolStatus += buf;
    }
    std::snprintf(buf, sizeof(buf), "; memory type %u, granularity %llu KiB%s", type,
                  (unsigned long long)(mAllocGran >> 10), mAllocGranMeasured ? "" : " (assumed)");
    mTexPoolStatus += buf;
    VK_LOG("texture pools (VMA %u.%u.%u): %s", VK_VERSION_MAJOR(VMA_VERSION), VK_VERSION_MINOR(VMA_VERSION),
           VK_VERSION_PATCH(VMA_VERSION), mTexPoolStatus.c_str());
}

void VkDeviceContext::destroyTexturePools()
{
    if (!mVma) { mTexPools.clear(); return; }
    // Lost device (#135): leak to process exit, as every other teardown does.
    if (vkDeviceIsLost()) { mVma = nullptr; mTexPools.clear(); return; }
    std::vector<VmaAllocation> left;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        for (const auto& kv : mPooled) left.push_back(kv.second.alloc);
    }
    if (!left.empty())
        std::fprintf(stderr, "[vkgfx] texture pools: %zu pooled images still live at shutdown (freed)\n", left.size());
    for (VmaAllocation a : left) vmaFreeMemory(mVma, a);
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        mPooled.clear();
        mTexBlocks.clear();
    }
    for (const TexPoolDef& p : mTexPools) vmaDestroyPool(mVma, p.pool);
    mTexPools.clear();
    vmaDestroyAllocator(mVma);
    mVma = nullptr;
}

int VkDeviceContext::texPoolFor(uint64_t size, uint64_t alignment) const
{
    if (!mVma || !size) return -1;
    const uint64_t a = alignment ? alignment : 1;
    const uint32_t n = (uint32_t)mTexPools.size();
    for (uint32_t i = 0; i < n; ++i)
    {
        const TexPoolDef& p = mTexPools[i];
        if (p.classBytes == 0) return size <= mTexMediumMax ? (int)i : -1;   // the medium pool (last)
        if (size > p.classBytes || p.classBytes % a != 0) continue;
        // Medium slab classes (#587): only within 1/8 rounding waste beyond what the
        // image's own alignment costs anywhere (the TLSF fallback places it at an aligned
        // offset too); an image no class fits that closely goes to the fallback pool.
        const uint64_t aligned = (size + a - 1) / a * a;
        if (p.classBytes > mTexSmallMax && p.classBytes > aligned && p.classBytes - aligned > size / 8) continue;
        return (int)i;
    }
    return -1;
}

bool VkDeviceContext::allocPooledImage(GpuImage& img, const VkMemoryRequirements& req, uint8_t cat, uint32_t w,
                                       uint32_t h, VkFormat fmt, const void* caller)
{
    if (!mVma || mTexPoolType == ~0u || !(req.memoryTypeBits & (1u << mTexPoolType))) return false;
    const int pi = texPoolFor(req.size, req.alignment);
    if (pi < 0) return false;
    const TexPoolDef& p = mTexPools[(size_t)pi];
    VkMemoryRequirements r = req;
    if (p.classBytes) r.size = p.classBytes;   // a slab slot: every allocation of the class is one size
    r.memoryTypeBits = 1u << mTexPoolType;
    VmaAllocationCreateInfo ci{};
    ci.pool = p.pool;
    ci.flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_MEMORY_BIT;   // fullest block first
    // #695 UMA, pools on the carve-out: no NEW block past its budget (existing blocks keep
    // filling) -- the dedicated path then spills the image to the host heap.
    if (mUma && mMemProps.memoryTypes[mTexPoolType].heapIndex == mUmaDevHeap)
    {
        ci.flags |= VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        if (!umaCarveFits(p.blockSize)) ci.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
    }
    // #614 compaction move: existing blocks only (fullest first; never a new block).
    const std::vector<VkDeviceMemory>* compact = t_texCompact;
    if (compact) ci.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
    VmaAllocation a = nullptr;
    VmaAllocationInfo info{};
    // Pool full and no new block (out of memory): the dedicated path decides (soft fail).
    if (vmaAllocateMemory(mVma, &r, &ci, &a, &info) != VK_SUCCESS)
    {
        if (compact) { std::lock_guard<std::mutex> lk(mAllocMutex.m); ++mTexCompactPlaceFail; }
        return false;
    }
    if (compact)
    {
        // The fullest block with room is a source being evacuated, or an empty block: no
        // other block has room -- moving there frees nothing. (Never bound: free at once.)
        bool ok = std::find(compact->begin(), compact->end(), info.deviceMemory) == compact->end();
        {
            std::lock_guard<std::mutex> lk(mAllocMutex.m);
            auto b = mTexBlocks.find(info.deviceMemory);
            ok = ok && b != mTexBlocks.end() && b->second.images > 0;
            if (!ok) ++mTexCompactPlaceFail;
        }
        if (!ok) { vmaFreeMemory(mVma, a); return false; }
    }
    if (vmaBindImageMemory(mVma, a, img.image) != VK_SUCCESS)
    {
        vmaFreeMemory(mVma, a);
        return false;
    }
    img.memory = info.deviceMemory;
    uint64_t va = 0;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        auto b = mAllocs.find(info.deviceMemory);
        if (b != mAllocs.end() && b->second.va) va = b->second.va + info.offset;
        mPooled[img.image] = PooledRec{a, (uint64_t)req.size, (uint32_t)pi, cat, info.deviceMemory, (uint64_t)info.size};
        TexBlockRec& tb = mTexBlocks[info.deviceMemory];   // #614
        tb.pool = (uint32_t)pi; ++tb.images; tb.used += info.size;
        tb.last_alloc = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        (mTexPoolDev ? mCatDev[cat] : mCatHost[cat]) += req.size;
        ++mCatCount[cat];
        mCatPeak[cat] = std::max(mCatPeak[cat], mCatHost[cat] + mCatDev[cat]);
        mCatPeakInt[cat] = std::max(mCatPeakInt[cat], mCatHost[cat] + mCatDev[cat]);
        mTexPoolLive[pi] += req.size;
        mTexPoolLiveTotal += req.size;
        ++mTexSubAllocCalls;
        if (req.size > mTexSmallMax)
        {   // #587: medium-range image -- slab class hit or TLSF fallback (miss)
            mTexMedAlignMax = std::max<uint64_t>(mTexMedAlignMax, req.alignment);
            ++(p.classBytes ? mTexClassHit : mTexClassMiss);
        }
    }
    vkImgTrack(img.image, va, req.size, w, h, 1, fmt, caller);
    return true;
}

VmaAllocation VkDeviceContext::releasePooled(VkImage image)
{
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    if (mPooled.empty()) return nullptr;
    auto it = mPooled.find(image);
    if (it == mPooled.end()) return nullptr;
    const PooledRec r = it->second;
    mPooled.erase(it);
    auto tb = mTexBlocks.find(r.block);   // #614
    if (tb != mTexBlocks.end())
    {
        tb->second.used -= std::min(tb->second.used, r.sub);
        if (tb->second.images <= 1) mTexBlocks.erase(tb);
        else --tb->second.images;
    }
    uint64_t& cb = mTexPoolDev ? mCatDev[r.cat] : mCatHost[r.cat];
    cb -= std::min(cb, r.bytes);
    if (mCatCount[r.cat]) --mCatCount[r.cat];
    mTexPoolLive[r.pool] -= std::min(mTexPoolLive[r.pool], r.bytes);
    mTexPoolLiveTotal -= std::min(mTexPoolLiveTotal, r.bytes);
    ++mTexSubFreeCalls;
    return r.alloc;
}

// A VMA pool block was allocated / freed (VmaDeviceMemoryCallbacks; runs inside VMA with
// the pool locked, on the allocating thread). The block is a tracked allocation of its
// own -- heap / device-local / allocation count / page padding -- but carries no category
// bytes: the images in it are counted per category as suballocations.
void VkDeviceContext::onVmaBlock(bool alloc, uint32_t memType, VkDeviceMemory mem, uint64_t bytes)
{
    if (!alloc) { vkMemBlockUntrack(mem); trackFree(mem); return; }
    if (memType >= mMemProps.memoryTypeCount) return;
    const VkDeviceAddress va = probeMemoryVA(mDevice, mem, memType, bytes);
    vkMemBlockTrack(mem, va, bytes, "texture pool block");   // #845 / #917
    const uint32_t heap = mMemProps.memoryTypes[memType].heapIndex;
    const bool host = (mMemProps.memoryTypes[memType].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool devHeap = (mMemProps.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
    const bool dev = mUma ? inBudgetHeap(heap) : devHeap;   // #695 (as trackAlloc)
    const uint64_t g = mAllocGran ? mAllocGran : 1;
    const uint32_t pad = devHeap ? (uint32_t)((bytes + g - 1) / g * g - bytes) : 0u;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    AllocRec rec{bytes, heap, true, host, (uint8_t)VKMEM_TEXTURE, false, dev, pad, nullptr};
    rec.pool = true;
    rec.va = va;
    mAllocs[mem] = rec;
    mPadBytes += pad;
    mHeapUsed[heap] += bytes;
    mImgBytes += bytes;
    if (host) mHostBytes += bytes;
    if (dev)
    {
        mDevLocalBytes += bytes;
        mDevLocalPeak = std::max(mDevLocalPeak, mDevLocalBytes);
    }
    mTexPoolBlockBytes += bytes;
    ++mTexPoolBlocks;
    ++mTexDevAllocCalls;
}

// ---------------------------------------------------------------------------
// TEXTURE POOL COMPACTION (#614): block choice + destination steering (the renderer moves)
// ---------------------------------------------------------------------------
uint64_t VkDeviceContext::texPoolReclaimOf(uint32_t pool, uint32_t blocks, uint32_t images, uint64_t used) const
{
    if (pool >= mTexPools.size() || !blocks) return 0;
    const TexPoolDef& p = mTexPools[pool];
    if (!p.blockSize) return 0;
    uint64_t need;
    if (p.classBytes && p.slots) need = (images + p.slots - 1) / p.slots;
    else need = (used + p.blockSize - 1) / p.blockSize;
    // (fallback: TLSF packing is not perfect -- one block of margin)
    if (!p.classBytes && blocks > need) ++need;
    return blocks > need ? (uint64_t)(blocks - need) * p.blockSize : 0;
}

uint64_t VkDeviceContext::texPoolReclaimable(uint32_t keepBlocks) const
{
    if (!mVma) return 0;
    uint32_t ne[MemStats::kTexPoolMax] = {}, imgs[MemStats::kTexPoolMax] = {};
    uint64_t used[MemStats::kTexPoolMax] = {};
    const uint32_t n = (uint32_t)std::min<size_t>(mTexPools.size(), MemStats::kTexPoolMax);
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        for (const auto& kv : mTexBlocks)
        {
            const uint32_t p = kv.second.pool;
            if (p >= n) continue;
            ++ne[p]; imgs[p] += kv.second.images; used[p] += kv.second.used;
        }
    }
    uint64_t r = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint64_t ri = texPoolReclaimOf(i, ne[i], imgs[i], used[i]);
        const uint64_t keep = (uint64_t)keepBlocks * mTexPools[i].blockSize;   // (headroom, texCompactPlan)
        r += ri > keep ? ri - keep : 0;
    }
    return r;
}

VkDeviceMemory VkDeviceContext::texPoolBlockOf(VkImage image) const
{
    if (!image) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    auto it = mPooled.find(image);
    return it == mPooled.end() ? VK_NULL_HANDLE : it->second.block;
}

void VkDeviceContext::texCompactPlan(uint64_t maxBytes, float maxOccupancy, const std::vector<VkDeviceMemory>& avoid,
                                     const std::function<int(VkImage)>& movable, TexCompactPick& out,
                                     uint32_t keepBlocks, double minIdleSec) const
{
    out = TexCompactPick{};
    if (!mVma || mTexPools.empty()) return;
    const uint32_t n = (uint32_t)std::min<size_t>(mTexPools.size(), MemStats::kTexPoolMax);
    struct Blk { VkDeviceMemory mem; uint32_t pool; uint32_t images; uint64_t used; double last_alloc; };
    std::vector<Blk> blocks;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        blocks.reserve(mTexBlocks.size());
        for (const auto& kv : mTexBlocks)
            if (kv.second.pool < n)
                blocks.push_back(Blk{kv.first, kv.second.pool, kv.second.images, kv.second.used, kv.second.last_alloc});
    }
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // Per pool: how many of its emptiest blocks a perfect packing frees (k); those at most
    // maxOccupancy full are the candidates. Choosing no more than k per pool keeps room for
    // their images in the pool's other blocks (slab: by slots; fallback: one block margin).
    std::vector<Blk> cand;
    {
        // (blocks still being evacuated count as used -- their old images live until their
        // frames finish -- and as already reclaimed: they take k's place)
        uint32_t ne[MemStats::kTexPoolMax] = {}, imgs[MemStats::kTexPoolMax] = {}, evac[MemStats::kTexPoolMax] = {};
        uint64_t used[MemStats::kTexPoolMax] = {};
        for (const Blk& b : blocks)
        {
            ++ne[b.pool]; imgs[b.pool] += b.images; used[b.pool] += b.used;
            if (std::find(avoid.begin(), avoid.end(), b.mem) != avoid.end()) ++evac[b.pool];
        }
        std::sort(blocks.begin(), blocks.end(), [](const Blk& a, const Blk& b)
                  { return a.pool != b.pool ? a.pool < b.pool : a.used < b.used; });
        size_t i = 0;
        while (i < blocks.size())
        {
            const uint32_t p = blocks[i].pool;
            const TexPoolDef& def = mTexPools[p];
            const uint64_t reclaim = texPoolReclaimOf(p, ne[p], imgs[p], used[p]);
            uint64_t k = def.blockSize ? reclaim / def.blockSize : 0;
            // (background: the pool keeps keepBlocks of slack as its allocation headroom)
            k = k > keepBlocks ? k - keepBlocks : 0;
            k = k > evac[p] ? k - evac[p] : 0;
            for (; i < blocks.size() && blocks[i].pool == p; ++i)
            {
                if (!k) continue;
                const Blk& b = blocks[i];
                if ((double)b.used > (double)maxOccupancy * (double)def.blockSize) continue;
                if (std::find(avoid.begin(), avoid.end(), b.mem) != avoid.end()) continue;
                // (background: a block allocated into recently is filling -- the pool's
                // newest block, or a destination of earlier moves -- not a source)
                if (minIdleSec > 0.0 && now - b.last_alloc < minIdleSec) continue;
                cand.push_back(b);
                --k;
            }
        }
    }
    if (cand.empty()) return;
    // Cheapest per byte freed first, over all pools: the bytes copied plus a CPU cost per
    // image moved (an image, views and a suballocation: counted as 64 KiB of copy), over
    // the block's size -- a sparse block of 1.4 MB images before one of 2 KiB images.
    auto cost = [&](const Blk& b)
    {
        return ((double)b.images * 65536.0 + (double)b.used) / (double)std::max<uint64_t>(mTexPools[b.pool].blockSize, 1);
    };
    std::sort(cand.begin(), cand.end(), [&](const Blk& a, const Blk& b) { return cost(a) < cost(b); });
    // Generous: a few blocks past maxBytes, so a pinned / busy one does not end the plan.
    {
        uint64_t sum = 0;
        size_t keep = 0;
        while (keep < cand.size() && (keep == 0 || sum < maxBytes * 2)) sum += cand[keep++].used;
        cand.resize(keep);
    }
    // The candidates' images (one pass over the pooled table, outside VMA).
    std::vector<std::vector<std::pair<VkImage, uint64_t>>> imgsOf(cand.size());
    {
        std::unordered_map<VkDeviceMemory, size_t> idx;
        for (size_t c = 0; c < cand.size(); ++c) idx[cand[c].mem] = c;
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        for (const auto& kv : mPooled)
        {
            auto f = idx.find(kv.second.block);
            if (f != idx.end()) imgsOf[f->second].emplace_back(kv.first, kv.second.sub);
        }
    }
    for (size_t c = 0; c < cand.size() && out.bytes < maxBytes; ++c)
    {
        bool pinned = false, busy = false;
        for (const auto& im : imgsOf[c])
        {
            const int m = movable(im.first);
            if (m == 0) { pinned = true; break; }
            if (m == 2) busy = true;
        }
        if (pinned) { ++out.pinnedBlocks; continue; }
        if (busy) { ++out.busyBlocks; continue; }
        if (imgsOf[c].empty()) continue;
        out.blocks.push_back(cand[c].mem);
        for (const auto& im : imgsOf[c])
        {
            out.images.push_back(im.first);
            out.blockOf.push_back((uint32_t)out.blocks.size() - 1u);
            out.bytes += im.second;
        }
    }
}

uint64_t VkDeviceContext::texPoolReleaseEmpty()
{
    if (!mVma || vkDeviceIsLost()) return 0;
    // (no mAllocMutex here: the block free callback takes it)
    uint64_t total = 0, blocks = 0;
    for (const TexPoolDef& p : mTexPools)
    {
        size_t nb = 0;
        VkDeviceSize b = 0;
        vmaReleasePoolEmptyBlocks(mVma, p.pool, &nb, &b);
        total += b; blocks += nb;
    }
    if (blocks)
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        mTexCompactReleased += blocks;
        mTexCompactReleasedBytes += total;
    }
    return total;
}

void VkDeviceContext::noteTexCompact(uint32_t moves, uint64_t bytes, uint32_t skippedBusy, uint32_t evacuated)
{
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    mTexCompactMoves += moves;
    mTexCompactMoved += bytes;
    mTexCompactSkipped += skippedBusy;
    mTexCompactEvacuated += evacuated;
}

GpuImage VkDeviceContext::createImage2D(uint32_t w, uint32_t h,
                                        VkFormat format,
                                        VkImageUsageFlags usage,
                                        VkImageAspectFlags aspect,
                                        uint32_t mipLevels,
                                        VkFormat mutableAltFormat, const char* site) {
    GpuImage img{};
    img.format = format;
    img.extent = {w, h};

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const VkFormat fmtList[2] = { format, mutableAltFormat };
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (mutableAltFormat != VK_FORMAT_UNDEFINED && mutableAltFormat != format) {
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        fl.viewFormatCount = 2;
        fl.pViewFormats = fmtList;
        ci.pNext = &fl;
    }
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        const VkResult r = vkCreateImage(mDevice, &ci, nullptr, &img.image);
        if (r != VK_SUCCESS && softFail(r, "createImage2D", (uint64_t)w * h * 4)) return GpuImage{};
        VK_CHECK(r);
    }

    // TEXTURE POOLS (#411): a texture / UI image (explicit VkMemScope) that is only
    // sampled / copied, small enough and not wanted dedicated by the driver is
    // suballocated from a VMA pool; everything else keeps its own allocation.
    const int scopeCat = t_memCat;
    const bool poolable = mVma && (scopeCat == (int)VKMEM_TEXTURE || scopeCat == (int)VKMEM_UI)
                          && !(usage & ~kPoolableUsage);
    VkMemoryRequirements req;
    bool pooled = false;
    if (poolable)
    {
        VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        r2.pNext = &dr;
        VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        ri.image = img.image;
        vkGetImageMemoryRequirements2(mDevice, &ri, &r2);
        req = r2.memoryRequirements;
        if (dr.prefersDedicatedAllocation || dr.requiresDedicatedAllocation)
        {
            std::lock_guard<std::mutex> lk(mAllocMutex.m);
            ++mTexDedicatedWanted;
        }
        else
            pooled = allocPooledImage(img, req, (uint8_t)scopeCat, w, h, format, __builtin_return_address(0));
    }
    else
        vkGetImageMemoryRequirements(mDevice, img.image, &req);
    if (!pooled && t_texCompact)
    {
        // #614 compaction move: a fuller pool block or nothing (a dedicated allocation
        // would cost more than the slot it frees). Not a soft failure: nothing ran out.
        vkDestroyImage(mDevice, img.image, nullptr);
        return GpuImage{};
    }
    if (!pooled)
    {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =   // (#695: the UMA host heap once the carve-out is full)
            pickDeviceLocalType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.size);
        VkMemoryAllocateFlagsInfo vaFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        vaFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (imageVaWanted()) ai.pNext = &vaFlags;   // (image registry: the memory's VA)
        if (!heapBudgetPreflight(ai.memoryTypeIndex, req.size, "createImage2D (heap budget pre-flight)")) {
            vkDestroyImage(mDevice, img.image, nullptr);
            return GpuImage{};
        }
        {
            const VkResult r = allocateMemoryUma(ai, &img.memory, "createImage2D", req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (r != VK_SUCCESS && softFail(r, "createImage2D", req.size)) {
                vkDestroyImage(mDevice, img.image, nullptr);
                return GpuImage{};
            }
            VK_CHECK(r);
        }
        trackAlloc(img.memory, req.size, ai.memoryTypeIndex, true, (uint32_t)usage, false, site);
        {   // #697: soft mode -- out of memory binding frees the image + its memory
            const VkResult r = vkBindImageMemory(mDevice, img.image, img.memory, 0);
            if (r != VK_SUCCESS && softFail(r, "createImage2D(bind)", req.size)) { destroyImage(img); return GpuImage{}; }
            VK_CHECK(r);
        }
        vkImgTrack(img.image, probeMemoryVA(mDevice, img.memory, ai.memoryTypeIndex, req.size), req.size, w, h, 1,
                   format, __builtin_return_address(0));
    }

    // A transfer-only image (copy staging: the Photo Studio scope readback, the texture
    // upload mip transient) cannot have a view (VUID-VkImageViewCreateInfo-image-04441);
    // nothing samples it, so it gets none (img.view stays null; destroyImage skips it).
    if (!(usage & ~(VkImageUsageFlags)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)))
        return img;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, mipLevels, 0, 1};
    {
        // #697: soft mode -- an out-of-memory view (driver host memory) frees the image too
        // and returns empty, like an allocation failure (+ SLVkInjectOomCalls).
        const VkResult r = (t_softAlloc && vkInjectOomTake()) ? VK_ERROR_OUT_OF_HOST_MEMORY
                         : vkCreateImageView(mDevice, &vi, nullptr, &img.view);
        if (r != VK_SUCCESS && softFail(r, "createImage2D(view)", 0)) {
            img.view = VK_NULL_HANDLE;
            destroyImage(img);
            vkLowMemNote(VKLM_VIEW, false);
            return GpuImage{};
        }
        VK_CHECK(r);
    }
    return img;
}

GpuImage VkDeviceContext::createImage3D(uint32_t w, uint32_t h, uint32_t d, VkFormat format,
                                        VkImageUsageFlags usage, const char* site) {
    GpuImage img{};
    img.format = format;
    img.extent = {w, h};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_3D;
    ci.format = format;
    ci.extent = {w, h, d};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        const VkResult r = vkCreateImage(mDevice, &ci, nullptr, &img.image);
        if (r != VK_SUCCESS && softFail(r, "createImage3D", (uint64_t)w * h * d * 4)) return GpuImage{};
        VK_CHECK(r);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(mDevice, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = pickDeviceLocalType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.size);   // #695
    VkMemoryAllocateFlagsInfo vaFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    vaFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (imageVaWanted()) ai.pNext = &vaFlags;
    if (!heapBudgetPreflight(ai.memoryTypeIndex, req.size, "createImage3D (heap budget pre-flight)")) {
        vkDestroyImage(mDevice, img.image, nullptr);
        return GpuImage{};
    }
    {
        const VkResult r = allocateMemoryUma(ai, &img.memory, "createImage3D", req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (r != VK_SUCCESS && softFail(r, "createImage3D", req.size)) {
            vkDestroyImage(mDevice, img.image, nullptr);
            return GpuImage{};
        }
        VK_CHECK(r);
    }
    trackAlloc(img.memory, req.size, ai.memoryTypeIndex, true, (uint32_t)usage, false, site);
    {   // #697: soft mode -- out of memory binding frees the image + its memory
        const VkResult r = vkBindImageMemory(mDevice, img.image, img.memory, 0);
        if (r != VK_SUCCESS && softFail(r, "createImage(bind)", req.size)) { destroyImage(img); return GpuImage{}; }
        VK_CHECK(r);
    }
    vkImgTrack(img.image, probeMemoryVA(mDevice, img.memory, ai.memoryTypeIndex, req.size), req.size, w, h, d,
               format, __builtin_return_address(0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    {
        const VkResult r = (t_softAlloc && vkInjectOomTake()) ? VK_ERROR_OUT_OF_HOST_MEMORY
                         : vkCreateImageView(mDevice, &vi, nullptr, &img.view);
        if (r != VK_SUCCESS && softFail(r, "createImage3D(view)", 0)) {   // #697 (as createImage2D)
            img.view = VK_NULL_HANDLE;
            destroyImage(img);
            vkLowMemNote(VKLM_VIEW, false);
            return GpuImage{};
        }
        VK_CHECK(r);
    }
    return img;
}

GpuImage VkDeviceContext::createImageLayers(uint32_t w, uint32_t h, uint32_t layers,
                                            uint32_t mipLevels, VkFormat format,
                                            VkImageUsageFlags usage, VkImageAspectFlags aspect,
                                            bool cube_compatible, VkImageViewType viewType,
                                            const char* site, const VkImageExtra* extra) {
    GpuImage img{};
    img.format = format;
    img.extent = {w, h};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube_compatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (extra) {
        ci.flags |= extra->flags;
        if (extra->viewFormatCount) {
            fl.viewFormatCount = extra->viewFormatCount;
            fl.pViewFormats = extra->viewFormats;
            ci.pNext = &fl;
        }
    }
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        // Soft allocation (#226): probe arrays / shadow cascades are setting-driven and
        // can be GBs; their callers step down instead of aborting.
        const VkResult r = vkCreateImage(mDevice, &ci, nullptr, &img.image);
        if (r != VK_SUCCESS && softFail(r, "createImageLayers", (uint64_t)w * h * layers * 8)) return GpuImage{};
        VK_CHECK(r);
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(mDevice, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = pickDeviceLocalType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, req.size);   // #695
    VkMemoryAllocateFlagsInfo vaFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    vaFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (imageVaWanted()) ai.pNext = &vaFlags;
    if (!heapBudgetPreflight(ai.memoryTypeIndex, req.size, "createImageLayers (heap budget pre-flight)")) {
        vkDestroyImage(mDevice, img.image, nullptr);
        return GpuImage{};
    }
    {
        const VkResult r = allocateMemoryUma(ai, &img.memory, "createImageLayers", req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (r != VK_SUCCESS && softFail(r, "createImageLayers", req.size)) {
            vkDestroyImage(mDevice, img.image, nullptr);
            return GpuImage{};
        }
        VK_CHECK(r);
    }
    trackAlloc(img.memory, req.size, ai.memoryTypeIndex, true, (uint32_t)usage, false, site);
    {   // #697: soft mode -- out of memory binding frees the image + its memory
        const VkResult r = vkBindImageMemory(mDevice, img.image, img.memory, 0);
        if (r != VK_SUCCESS && softFail(r, "createImage(bind)", req.size)) { destroyImage(img); return GpuImage{}; }
        VK_CHECK(r);
    }
    vkImgTrack(img.image, probeMemoryVA(mDevice, img.memory, ai.memoryTypeIndex, req.size), req.size, w, h, layers,
               format, __builtin_return_address(0));
    if (t_softAlloc)
    {
        // #697: soft mode -- an out-of-memory default view frees the image (createViewSoft).
        img.view = createViewSoft(img.image, format, viewType, aspect, 0, mipLevels, 0, layers,
                                  extra ? extra->defaultViewUsage : 0);
        if (!img.view) { destroyImage(img); vkLowMemNote(VKLM_VIEW, false); return GpuImage{}; }
        return img;
    }
    img.view = createView(img.image, format, viewType, aspect, 0, mipLevels, 0, layers,
                          extra ? extra->defaultViewUsage : 0);
    return img;
}

uint64_t VkDeviceContext::imageMemoryBytes(uint32_t w, uint32_t h, uint32_t layers, uint32_t mipLevels,
                                           VkFormat format, VkImageUsageFlags usage, bool cube_compatible,
                                           const VkImageExtra* extra) const {
    if (!mDevice || !w || !h) return 0;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube_compatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    if (extra) {
        ci.flags |= extra->flags;
        if (extra->viewFormatCount) {
            fl.viewFormatCount = extra->viewFormatCount;
            fl.pViewFormats = extra->viewFormats;
            ci.pNext = &fl;
        }
    }
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = std::max(mipLevels, 1u);
    ci.arrayLayers = std::max(layers, 1u);
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceImageMemoryRequirements dr{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
    dr.pCreateInfo = &ci;
    VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(mDevice, &dr, &req);
    return (uint64_t)req.memoryRequirements.size;
}

VkImageView VkDeviceContext::createView(VkImage image, VkFormat format, VkImageViewType type,
                                        VkImageAspectFlags aspect, uint32_t baseMip,
                                        uint32_t mipCount, uint32_t baseLayer,
                                        uint32_t layerCount, VkImageUsageFlags viewUsage) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (viewUsage) {
        vu.usage = viewUsage;
        vi.pNext = &vu;
    }
    vi.image = image;
    vi.viewType = type;
    vi.format = format;
    vi.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkImageView v = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(mDevice, &vi, nullptr, &v));
    return v;
}

VkImageView VkDeviceContext::createViewSoft(VkImage image, VkFormat format, VkImageViewType type,
                                            VkImageAspectFlags aspect, uint32_t baseMip,
                                            uint32_t mipCount, uint32_t baseLayer,
                                            uint32_t layerCount, VkImageUsageFlags viewUsage) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (viewUsage) {
        vu.usage = viewUsage;
        vi.pNext = &vu;
    }
    vi.image = image;
    vi.viewType = type;
    vi.format = format;
    vi.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkImageView v = VK_NULL_HANDLE;
    const VkResult r = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY : vkCreateImageView(mDevice, &vi, nullptr, &v);
    if (vkIsOom(r))
    {
        const bool prev = t_softAlloc;   // (counted as a soft failure whatever the mode)
        t_softAlloc = true;
        softFail(r, "createView", 0);
        t_softAlloc = prev;
        return VK_NULL_HANDLE;
    }
    VK_CHECK(r);
    return v;
}

void VkDeviceContext::destroyImage(GpuImage& img) {
    if (vkDeviceIsLost()) { img = GpuImage{}; return; }
    if (img.view) vkDestroyImageView(mDevice, img.view, nullptr);   // (hook: untracks the view)
    // A pooled image (#411) is found by its VkImage (callers may hand back only
    // {image, memory}); its memory is a shared block: free the suballocation -- a
    // free-list operation; vkFreeMemory happens only when VMA releases an empty block.
    const VmaAllocation pooled = img.image ? releasePooled(img.image) : nullptr;
    // An aliased target (#615): its block is shared and refcounted -- drop this image's
    // reference after the image is gone (the last one frees the block).
    VkDeviceMemory aliasBlock = VK_NULL_HANDLE;
    if (img.image)
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        if (!mAliasImages.empty())
        {
            auto a = mAliasImages.find(img.image);
            if (a != mAliasImages.end()) { aliasBlock = a->second; mAliasImages.erase(a); }
        }
    }
    if (img.image) { vkImgUntrack(img.image); vkDestroyImage(mDevice, img.image, nullptr); }
    if (aliasBlock) aliasUnref(aliasBlock);
    if (pooled) vmaFreeMemory(mVma, pooled);
    else if (img.memory)
    {
        // Never vkFreeMemory a texture pool block from here: other images live in it and
        // VMA owns it. Reaching this means a GpuImage lost its VkImage (or was destroyed
        // twice) while keeping the block -- log it and leave the block alone.
        bool block = false;
        {
            std::lock_guard<std::mutex> lk(mAllocMutex.m);
            auto it = mAllocs.find(img.memory);
            block = it != mAllocs.end() && it->second.pool;
        }
        if (block)
        {
            static uint32_t s_logged = 0;
            if (s_logged++ < 8)
                std::fprintf(stderr, "[vkgfx] destroyImage: memory is a texture pool block but the image is not "
                                     "pooled (image 0x%llx) -- not freed\n", (unsigned long long)(uintptr_t)img.image);
        }
        else { trackFree(img.memory); vkFreeMemory(mDevice, img.memory, nullptr); }
    }
    img = GpuImage{};
}

// ---- ALIASED TRANSIENT TARGETS (#615) ----
VkImage VkDeviceContext::createAliasImage2D(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                                            VkMemoryRequirements& req, bool& dedicated)
{
    req = VkMemoryRequirements{};
    dedicated = false;
    if (!mDevice || !w || !h) return VK_NULL_HANDLE;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    applySharing(ci.sharingMode, ci.queueFamilyIndexCount, ci.pQueueFamilyIndices, true, usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(mDevice, &ci, nullptr, &image) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 r2{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    r2.pNext = &dr;
    VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
    ri.image = image;
    vkGetImageMemoryRequirements2(mDevice, &ri, &r2);
    req = r2.memoryRequirements;
    // (prefersDedicatedAllocation is a hint -- NVIDIA sets it on render targets -- and
    // binding into a shared block stays valid; only a requirement rules sharing out.)
    dedicated = dr.requiresDedicatedAllocation == VK_TRUE;
    return image;
}

VkDeviceMemory VkDeviceContext::allocAliasBlock(VkDeviceSize bytes, uint32_t typeBits, uint32_t& typeIndex)
{
    // (findMemoryType aborts on no match: pick here, and fail softly instead.)
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < mMemProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        { type = i; break; }
    if (type == ~0u || !bytes || !mDevice) return VK_NULL_HANDLE;
    SoftAllocScope soft(this);   // a failure falls back to plain per-image allocations
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    VkMemoryAllocateFlagsInfo vaFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    vaFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (imageVaWanted()) ai.pNext = &vaFlags;   // (image registry: the block's VA)
    if (!heapBudgetPreflight(type, bytes, "allocAliasBlock (heap budget pre-flight)")) return VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    const VkResult r = allocateMemory(ai, &mem, "allocAliasBlock");
    if (r != VK_SUCCESS)
    {
        softFail(r, "allocAliasBlock", bytes);
        return VK_NULL_HANDLE;
    }
    trackAlloc(mem, bytes, type, true, (uint32_t)VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, "allocAliasBlock");
    const uint64_t va = probeMemoryVA(mDevice, mem, type, bytes);
    vkMemBlockTrack(mem, va, bytes, "alias target block");   // #845 / #917
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        mAliasBlocks[mem] = AliasBlockRec{1u, va, (uint64_t)bytes};
    }
    typeIndex = type;
    return mem;
}

bool VkDeviceContext::bindAliasImage(VkImage image, VkDeviceMemory block, VkDeviceSize offset, VkDeviceSize bytes,
                                     uint32_t w, uint32_t h, VkFormat format, const void* caller)
{
    if (!image || !block) return false;
    uint64_t va = 0;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        auto b = mAliasBlocks.find(block);
        if (b == mAliasBlocks.end() || !b->second.refs || offset + bytes > b->second.bytes) return false;
        va = b->second.va ? b->second.va + offset : 0;
    }
    if (vkBindImageMemory(mDevice, image, block, offset) != VK_SUCCESS) return false;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        ++mAliasBlocks[block].refs;
        mAliasImages[image] = block;
    }
    // (Several images share this VA range: the registry keeps the latest per address.)
    vkImgTrack(image, va, bytes, w, h, 1, format, caller);
    return true;
}

void VkDeviceContext::aliasUnref(VkDeviceMemory block)
{
    bool last = false;
    {
        std::lock_guard<std::mutex> lk(mAllocMutex.m);
        auto b = mAliasBlocks.find(block);
        if (b == mAliasBlocks.end()) return;
        if (b->second.refs) --b->second.refs;
        if (!b->second.refs) { mAliasBlocks.erase(b); last = true; }
    }
    if (last) vkMemBlockUntrack(block);   // #845 / #917
    if (last && !vkDeviceIsLost())
    {
        trackFree(block);
        vkFreeMemory(mDevice, block, nullptr);
    }
}

void VkDeviceContext::releaseAliasBlock(VkDeviceMemory block)
{
    if (block) aliasUnref(block);
}

VkDeviceMemory VkDeviceContext::aliasBlockOf(VkImage image) const
{
    if (!image) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    auto a = mAliasImages.find(image);
    return a == mAliasImages.end() ? VK_NULL_HANDLE : a->second;
}

void VkDeviceContext::aliasBlockStats(uint32_t& blocks, uint64_t& bytes) const
{
    std::lock_guard<std::mutex> lk(mAllocMutex.m);
    blocks = (uint32_t)mAliasBlocks.size();
    bytes = 0;
    for (const auto& kv : mAliasBlocks) bytes += kv.second.bytes;
}

void VkAliasArena::setup(const char* name, const uint8_t* phase, uint32_t slotCount)
{
    mName = name ? name : "alias";
    mSlotCount = std::min(slotCount, kMaxSlots);
    for (uint32_t s = 0; s < mSlotCount; ++s) mPhase[s] = (uint8_t)std::min<uint32_t>(phase[s], 7u);
}

bool VkAliasArena::live(const VkDeviceContext& ctx, uint32_t s) const
{
    return mSlots[s].image && ctx.aliasBlockOf(mSlots[s].image) != VK_NULL_HANDLE;
}

void VkAliasArena::dropBlock(VkDeviceContext& ctx)
{
    if (mBlock) ctx.releaseAliasBlock(mBlock);   // (its images keep it until they go)
    mBlock = VK_NULL_HANDLE;
    mBlockSize = 0;
    for (uint32_t s = 0; s < mSlotCount; ++s) { mSlots[s].off = 0; mSlots[s].region = 0; }
}

void VkAliasArena::setEnabled(VkDeviceContext& ctx, bool on)
{
    if (on == mEnabled) return;
    mEnabled = on;
    ++mEpoch;
    if (!on) dropBlock(ctx);
}

void VkAliasArena::trim(VkDeviceContext& ctx)
{
    if (!mBlock) return;
    for (uint32_t s = 0; s < mSlotCount; ++s)
        if (mSlots[s].image && ctx.aliasBlockOf(mSlots[s].image) == mBlock) return;
    dropBlock(ctx);
}

void VkAliasArena::forget(uint32_t first, uint32_t count)
{
    for (uint32_t s = first; s < mSlotCount && s < first + count; ++s) mSlots[s] = Slot{};
}

void VkAliasArena::shutdown(VkDeviceContext& ctx)
{
    dropBlock(ctx);
    for (uint32_t s = 0; s < mSlotCount; ++s) mSlots[s] = Slot{};
}

void VkAliasArena::stats(const VkDeviceContext& ctx, uint32_t& images, uint64_t& memberBytes, uint64_t& blockBytes,
                         uint32_t& phases) const
{
    images = 0; memberBytes = 0; blockBytes = mBlock ? (uint64_t)mBlockSize : 0; phases = 0;
    uint32_t mask = 0;
    for (uint32_t s = 0; s < mSlotCount; ++s)
        if (mBlock && mSlots[s].image && ctx.aliasBlockOf(mSlots[s].image) == mBlock)
        {
            ++images;
            memberBytes += mSlots[s].req.size;
            mask |= 1u << mPhase[s];
        }
    for (; mask; mask &= mask - 1) ++phases;
}

bool VkAliasArena::create(VkDeviceContext& ctx, const Desc* d, uint32_t n, GpuImage* out)
{
    mLastFail = "";
    if (!mEnabled) { mLastFail = "off"; return false; }
    if (!n || n > mSlotCount) { mLastFail = "bad set"; return false; }
    // 1. The set's images, unbound, and their requirements.
    VkImage img[kMaxSlots] = {};
    VkMemoryRequirements req[kMaxSlots] = {};
    bool inSet[kMaxSlots] = {};
    auto destroyUnbound = [&]() { for (uint32_t i = 0; i < n; ++i) if (img[i]) vkDestroyImage(ctx.device(), img[i], nullptr); };
    for (uint32_t i = 0; i < n; ++i)
    {
        bool dedicated = false;
        if (d[i].slot >= mSlotCount || inSet[d[i].slot]) { mLastFail = "bad slot"; destroyUnbound(); return false; }
        inSet[d[i].slot] = true;
        img[i] = ctx.createAliasImage2D(d[i].w, d[i].h, d[i].format, d[i].usage, req[i], dedicated);
        if (!img[i]) { mLastFail = "image creation failed"; destroyUnbound(); return false; }
        if (dedicated) { mLastFail = "driver requires a dedicated allocation"; destroyUnbound(); return false; }
    }
    // 2. Plan: the set's new requirements + every OTHER slot's last one -- live (it keeps
    //    its region in a new block, where its owner re-creates it after the epoch moves) or
    //    dead but not forgotten (its owner normally comes back at that size: after an
    //    offscreen render at another size, a setting toggle) -- packed per phase.
    VkMemoryRequirements need[kMaxSlots] = {};
    bool use[kMaxSlots] = {};
    for (uint32_t i = 0; i < n; ++i) { need[d[i].slot] = req[i]; use[d[i].slot] = true; }
    for (uint32_t s = 0; s < mSlotCount; ++s)
        if (!use[s] && (mSlots[s].req.size || live(ctx, s))) { need[s] = mSlots[s].req; use[s] = true; }
    VkDeviceSize cursor[8] = {}, planOff[kMaxSlots] = {}, total = 0;
    uint32_t bits = ~0u;
    for (uint32_t s = 0; s < mSlotCount; ++s)
    {
        if (!use[s]) continue;
        const VkDeviceSize a = std::max<VkDeviceSize>(need[s].alignment, 1);
        VkDeviceSize& c = cursor[mPhase[s]];
        planOff[s] = (c + a - 1) / a * a;
        c = planOff[s] + need[s].size;
        total = std::max(total, c);
        bits &= need[s].memoryTypeBits;
    }
    // 3. The current block takes the set if every member has a big-enough, aligned region
    //    of a compatible type, and it is not far bigger than the plan (a shrink, the end of
    //    a large configuration) -- then a right-sized block replaces it.
    bool fits = mBlock != VK_NULL_HANDLE && mBlockSize <= total + total / 2 + (16ull << 20);
    for (uint32_t i = 0; fits && i < n; ++i)
    {
        const Slot& sl = mSlots[d[i].slot];
        const VkDeviceSize a = std::max<VkDeviceSize>(req[i].alignment, 1);
        fits = sl.region >= req[i].size && sl.off % a == 0 && (req[i].memoryTypeBits & (1u << mBlockType));
    }
    if (!fits)
    {
        if (!bits) { mLastFail = "no memory type common to the set and the live images"; destroyUnbound(); return false; }
        uint32_t type = 0;
        const VkDeviceMemory blk = ctx.allocAliasBlock(total, bits, type);
        if (!blk) { mLastFail = "block allocation failed (or no device-local common type)"; destroyUnbound(); return false; }
        dropBlock(ctx);   // the old block lives on in its images until their owners re-create them
        mBlock = blk; mBlockSize = total; mBlockType = type;
        for (uint32_t s = 0; s < mSlotCount; ++s)
        {
            mSlots[s].off = use[s] ? planOff[s] : 0;
            mSlots[s].region = use[s] ? need[s].size : 0;
        }
        ++mEpoch;   // images on the old block: stale
    }
    // 4. Bind + views. A failure unwinds the whole set.
    for (uint32_t i = 0; i < n; ++i)
    {
        Slot& sl = mSlots[d[i].slot];
        if (!ctx.bindAliasImage(img[i], mBlock, sl.off, req[i].size, d[i].w, d[i].h, d[i].format,
                                __builtin_return_address(0)))
        {
            mLastFail = "bind failed";
            for (uint32_t k = 0; k < i; ++k) { ctx.destroyImage(out[k]); }
            for (uint32_t k = i; k < n; ++k) vkDestroyImage(ctx.device(), img[k], nullptr);
            return false;
        }
        GpuImage g{};
        g.image = img[i];
        g.memory = VK_NULL_HANDLE;   // shared: never freed through the GpuImage
        g.format = d[i].format;
        g.extent = {d[i].w, d[i].h};
        g.view = ctx.createView(img[i], d[i].format, VK_IMAGE_VIEW_TYPE_2D, d[i].aspect, 0, 1, 0, 1);
        out[i] = g;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        // (a recycled handle another slot still names is not that slot's image any more)
        for (uint32_t s = 0; s < mSlotCount; ++s) if (mSlots[s].image == img[i]) mSlots[s].image = VK_NULL_HANDLE;
        mSlots[d[i].slot].image = img[i];
        mSlots[d[i].slot].req = req[i];
    }
    return true;
}

void VkDeviceContext::destroyView(VkImageView& v) {
    if (vkDeviceIsLost()) { v = VK_NULL_HANDLE; return; }
    if (v) vkDestroyImageView(mDevice, v, nullptr);
    v = VK_NULL_HANDLE;
}

// "imm <file>:<line>" for a submitImmediate caller: interned (breadcrumb / submit-log names
// must outlive the call; the set of call sites is small and fixed).
static const char* immCrumbName(const char* file, int line, bool end)
{
    const char* base = file ? file : "?";
    for (const char* c = base; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s %s:%d", end ? "imm_end" : "imm", base, line);
    static std::mutex s_m;
    static std::unordered_set<std::string> s_names;
    std::lock_guard<std::mutex> lk(s_m);
    return s_names.insert(std::string(buf)).first->c_str();
}

void VkDeviceContext::submitImmediate(
    const std::function<void(VkCommandBuffer)>& record, const char* file, int line) {
    if (vkDeviceIsLost()) return;   // #135: no GPU work after a device loss
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mCommandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(mDevice, &ai, &cmd));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    // #845 / #917: out-of-frame work is breadcrumbed under its caller (a loss between
    // frames then names it), and its submission goes into the submit log the same way.
    const char* crumb = immCrumbName(file, line, false);
    vkBreadcrumb(cmd, crumb);
    record(cmd);
    vkBreadcrumb(cmd, immCrumbName(file, line, true));
    VK_CHECK(vkEndCommandBuffer(cmd));
    vkSubmitLogNote(mQueue, crumb, file, line);

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(mDevice, &fci, nullptr, &fence));

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(mQueue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(mDevice, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(mDevice, fence, nullptr);
    vkFreeCommandBuffers(mDevice, mCommandPool, 1, &cmd);
}

GpuBuffer VkDeviceContext::createDeviceLocalWithData(const void* data,
                                                     VkDeviceSize size,
                                                     VkBufferUsageFlags usage,
                                                     bool deviceAddress, const char* site) {
    GpuBuffer staging = createBuffer(
        size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false, site);
    std::memcpy(staging.mapped, data, (size_t)size);

    GpuBuffer dst = createBuffer(
        size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, deviceAddress, site);

    submitImmediate([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroyBuffer(staging);
    return dst;
}

// ---- descriptor / view hooks (vk_common.h routes every vkgfx caller here) ----
#undef vkUpdateDescriptorSets
#undef vkCmdBindDescriptorSets
#undef vkAllocateDescriptorSets
#undef vkCreateImageView
#undef vkDestroyImageView
void vkqUpdateDescriptorSets(VkDevice dev, uint32_t writeCount, const VkWriteDescriptorSet* writes,
                             uint32_t copyCount, const VkCopyDescriptorSet* copies)
{
    if (sDescTrack.load(std::memory_order_relaxed)) descTrackUpdate(writeCount, writes, copyCount, copies);
    if (tCowGuard || sCowTracked.load(std::memory_order_relaxed)) cowUpdate(writeCount, writes, copyCount, copies);
    vkUpdateDescriptorSets(dev, writeCount, writes, copyCount, copies);
}

void vkqCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout layout,
                              uint32_t firstSet, uint32_t setCount, const VkDescriptorSet* sets,
                              uint32_t dynCount, const uint32_t* dyn)
{
    if (sDescTrack.load(std::memory_order_relaxed) && sets) descTrackBind(setCount, sets);
    vkCmdBindDescriptorSets(cmd, bp, layout, firstSet, setCount, sets, dynCount, dyn);
}

VkResult vkqAllocateDescriptorSets(VkDevice dev, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* sets)
{
    const VkResult r = vkAllocateDescriptorSets(dev, ai, sets);
    if (r == VK_SUCCESS && ai && sets && sCowTracked.load(std::memory_order_relaxed))
    {
        // A recycled handle is a new, untracked set (vkCowTrack registers it afresh).
        std::lock_guard<std::mutex> lk(sCowM);
        for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) sCowMask.erase((uint64_t)(uintptr_t)sets[i]);
        sCowTracked.store((uint32_t)sCowMask.size(), std::memory_order_relaxed);
    }
    if (r == VK_SUCCESS && ai && sets && sDescTrack.load(std::memory_order_relaxed))
    {
        // A recycled set handle starts empty: forget the previous set's records.
        AddrRegistry& R = addrReg();
        std::lock_guard<std::mutex> lk(R.m);
        for (uint32_t i = 0; i < ai->descriptorSetCount; ++i) R.descSets.erase((uint64_t)(uintptr_t)sets[i]);
    }
    return r;
}

VkResult vkqCreateImageView(VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* cb,
                            VkImageView* out)
{
    const VkResult r = vkCreateImageView(dev, ci, cb, out);
    if (r == VK_SUCCESS && out && ci) vkViewTrack(*out, ci->image);
    return r;
}

void vkqDestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks* cb)
{
    vkViewUntrack(v);
    vkDestroyImageView(dev, v, cb);
}

// ---------------------------------------------------------------------------
// DEVICE LOST (#135): state, raise, simulation, wait / submit hooks (vk_common.h)
// ---------------------------------------------------------------------------
// At the END of the file on purpose: everything above (submitImmediate included) calls
// the hooked names, so it goes through the lost-state gate and the simulation too.
bool vkDeviceIsLost() { return sDeviceLost.load(std::memory_order_acquire); }
bool vkDeviceLostSimulated() { return sLostSimulated.load(std::memory_order_relaxed); }

const char* vkQueueName(VkQueue q)
{
    if (q == VK_NULL_HANDLE) return "?";
    if (q == sCrumbQueue) return "graphics";
    if (q == sCrumbQueue2) return "async compute";
    return "other";
}

void vkSimulateDeviceLost(int site)
{
    if (site < VK_SIM_LOST_SUBMIT || site > VK_SIM_LOST_PRESENT) site = VK_SIM_LOST_NONE;
    sSimSite.store(site, std::memory_order_release);
    if (site != VK_SIM_LOST_NONE)
    {
        static const char* const kSite[] = {"", "queue submit", "fence / semaphore wait", "present"};
        std::fprintf(stderr, "[vkgfx] RenderVKDebugSimulateDeviceLost: the next %s reports VK_ERROR_DEVICE_LOST\n",
                     kSite[site]);
    }
}

// Consumes an armed simulation for `site` (true once).
bool vkSimulateDeviceLostTake(int site)
{
    if (sSimSite.load(std::memory_order_relaxed) != site) return false;
    int want = site;
    if (!sSimSite.compare_exchange_strong(want, VK_SIM_LOST_NONE)) return false;
    sLostSimulated.store(true, std::memory_order_relaxed);
    return true;
}

void vkDeviceLostRaise(const char* call, VkResult result, const char* queue, const char* file, int line)
{
    // State FIRST: every other thread / later call stops issuing GPU work right away.
    if (result == VK_ERROR_DEVICE_LOST && !vkDeviceLostSimulated())
        sLostConfirmed.store(true, std::memory_order_release);
    sDeviceLost.store(true, std::memory_order_release);
    if (sLostRaised.exchange(true)) return;
    std::fprintf(stderr, "[vkgfx] GRAPHICS DEVICE LOST: %s = %s (%d)%s%s at %s:%d%s\n",
                 call ? call : "?", vkResultString(result), (int)result,
                 queue ? ", queue " : "", queue ? queue : "", file ? file : "?", line,
                 vkDeviceLostSimulated() ? " [SIMULATED]" : "");
    std::fflush(stderr);
    if (gVkDeviceLostHandler)
        gVkDeviceLostHandler(call, result, queue, file, line);
    else
        vkReportDeviceLost();
}

// ---- #845 / #917 submit log (vk_common.h vkSubmitLogNote) ----
namespace {
struct SubmitLogEntry
{
    std::atomic<const char*> what{nullptr};
    std::atomic<const char*> file{nullptr};
    std::atomic<int> line{0};
    std::atomic<int> queue{0};          // 0 graphics, 1 async compute, 2 other
    std::atomic<uint32_t> frame{0};     // address registry frame
    std::atomic<uint32_t> crumb{0};     // breadcrumb sequence at the submit (crumbs up to it were recorded before)
    std::atomic<double> ms{0.0};        // host time
};
constexpr uint32_t kSubmitLog = 64;
SubmitLogEntry sSubmitLog[kSubmitLog];
std::atomic<uint32_t> sSubmitLogSeq{0};
}

void vkSubmitLogNote(VkQueue q, const char* what, const char* file, int line)
{
    const uint32_t i = sSubmitLogSeq.fetch_add(1, std::memory_order_relaxed) % kSubmitLog;
    SubmitLogEntry& e = sSubmitLog[i];
    e.what.store(what, std::memory_order_relaxed);
    e.file.store(file, std::memory_order_relaxed);
    e.line.store(line, std::memory_order_relaxed);
    e.queue.store(q == sCrumbQueue ? 0 : (q == sCrumbQueue2 ? 1 : 2), std::memory_order_relaxed);
    e.frame.store(vkAddrCurrentFrame(), std::memory_order_relaxed);
    e.crumb.store(sCrumbSeq, std::memory_order_relaxed);
    e.ms.store(vkNowMs(), std::memory_order_relaxed);
}

static void vkReportSubmitLog()
{
    const uint32_t end = sSubmitLogSeq.load(std::memory_order_relaxed);
    const uint32_t n = std::min(end, kSubmitLog);
    const double now = vkNowMs();
    vkFaultLog("[vktest] last %u queue submissions (oldest first; crumb = breadcrumb sequence when submitted, "
               "async compute-only queue attachment transitions so far: %u):\n", n, vkAsyncAttachTransitions());
    static const char* const kQ[3] = {"graphics", "async", "other"};
    for (uint32_t k = 0; k < n; ++k)
    {
        const SubmitLogEntry& e = sSubmitLog[(end - n + k) % kSubmitLog];
        const char* f = e.file.load(std::memory_order_relaxed);
        const char* base = f ? f : "?";
        for (const char* c = base; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
        const char* w = e.what.load(std::memory_order_relaxed);
        vkFaultLog("[vktest]   %-8s %-18s %s:%d frame %u crumb #%u, %.1f ms ago\n", kQ[e.queue.load() % 3],
                   w ? w : "?", base, e.line.load(), e.frame.load(), e.crumb.load(), now - e.ms.load());
    }
}

// ---- #845 / #917 AS build inputs (vkBuildAsNoted) ----
namespace {
struct AsInputRec
{
    VkDeviceAddress addr = 0;
    VkDeviceSize extent = 0;   // bytes the build reads from addr (0 = unknown: start only)
    uint32_t frame = 0;        // registry frame
    uint8_t kind = 0;          // AsInputKind
    bool async = false;        // recorded into an async compute command buffer
    bool update = false;       // a refit (mode UPDATE)
    bool tlas = false;
};
enum AsInputKind : uint8_t { ASIN_VERTEX, ASIN_INDEX, ASIN_TRANSFORM, ASIN_INSTANCES, ASIN_AABBS, ASIN_SCRATCH };
const char* const kAsInputName[] = {"vertex", "index", "transform", "instances", "aabbs", "scratch"};
constexpr uint32_t kAsInputRing = 32768;
std::mutex sAsInputM;
std::vector<AsInputRec> sAsInputs;   // ring (sized on first use)
uint64_t sAsInputSeq = 0;
uint32_t indexBytes(VkIndexType t)
{
    return t == VK_INDEX_TYPE_UINT16 ? 2u : t == VK_INDEX_TYPE_UINT32 ? 4u : t == VK_INDEX_TYPE_UINT8_EXT ? 1u : 0u;
}
}

static void VKAPI_PTR vkBuildAsNoted(VkCommandBuffer cmd, uint32_t n,
                                     const VkAccelerationStructureBuildGeometryInfoKHR* infos,
                                     const VkAccelerationStructureBuildRangeInfoKHR* const* ranges)
{
    if (infos && ranges && n)
    {
        const uint32_t frame = vkAddrCurrentFrame();
        const bool async = vkAsyncIsAsyncCmd(cmd);
        std::lock_guard<std::mutex> lk(sAsInputM);
        if (sAsInputs.empty()) sAsInputs.resize(kAsInputRing);
        auto add = [&](uint8_t kind, VkDeviceAddress a, VkDeviceSize ext, const VkAccelerationStructureBuildGeometryInfoKHR& gi)
        {
            AsInputRec& r = sAsInputs[sAsInputSeq++ % kAsInputRing];
            r.addr = a; r.extent = ext; r.frame = frame; r.kind = kind; r.async = async;
            r.update = gi.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
            r.tlas = gi.type == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        };
        for (uint32_t i = 0; i < n; ++i)
        {
            const VkAccelerationStructureBuildGeometryInfoKHR& gi = infos[i];
            add(ASIN_SCRATCH, gi.scratchData.deviceAddress, 0, gi);
            for (uint32_t g = 0; g < gi.geometryCount && ranges[i]; ++g)
            {
                const VkAccelerationStructureGeometryKHR* geo = gi.pGeometries ? &gi.pGeometries[g]
                                                              : (gi.ppGeometries ? gi.ppGeometries[g] : nullptr);
                if (!geo) continue;
                const VkAccelerationStructureBuildRangeInfoKHR& rg = ranges[i][g];
                if (geo->geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR)
                {
                    const auto& t = geo->geometry.triangles;
                    const uint32_t ib = indexBytes(t.indexType);
                    if (ib)
                    {
                        add(ASIN_INDEX, t.indexData.deviceAddress + rg.primitiveOffset, (VkDeviceSize)rg.primitiveCount * 3u * ib, gi);
                        add(ASIN_VERTEX, t.vertexData.deviceAddress + (VkDeviceSize)rg.firstVertex * t.vertexStride,
                            ((VkDeviceSize)t.maxVertex + 1u) * t.vertexStride, gi);
                    }
                    else
                        add(ASIN_VERTEX, t.vertexData.deviceAddress + rg.primitiveOffset + (VkDeviceSize)rg.firstVertex * t.vertexStride,
                            (VkDeviceSize)rg.primitiveCount * 3u * t.vertexStride, gi);
                    if (t.transformData.deviceAddress)
                        add(ASIN_TRANSFORM, t.transformData.deviceAddress + rg.transformOffset, 48, gi);
                }
                else if (geo->geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR)
                    add(ASIN_INSTANCES, geo->geometry.instances.data.deviceAddress + rg.primitiveOffset,
                        geo->geometry.instances.arrayOfPointers ? (VkDeviceSize)rg.primitiveCount * 8u
                                                                : (VkDeviceSize)rg.primitiveCount * 64u, gi);
                else if (geo->geometryType == VK_GEOMETRY_TYPE_AABBS_KHR)
                    add(ASIN_AABBS, geo->geometry.aabbs.data.deviceAddress + rg.primitiveOffset,
                        (VkDeviceSize)rg.primitiveCount * geo->geometry.aabbs.stride, gi);
            }
        }
    }
    if (sRealBuildAs) sRealBuildAs(cmd, n, infos, ranges);
}

// Every input of the last 4 registry frames: its start AND its last byte must lie in one live
// buffer (or live AS storage). Anything else -- a zero / tiny address, a freed buffer, a range
// running past its buffer's end -- is printed (capped); a summary line always.
static void vkReportAsBuildInputs()
{
    std::unique_lock<std::mutex> lk(sAsInputM, std::try_to_lock);
    if (!lk.owns_lock()) { vkFaultLog("[vktest] AS build inputs: (busy)\n"); return; }
    if (sAsInputs.empty() || !sAsInputSeq) { vkFaultLog("[vktest] AS build inputs: none recorded\n"); return; }
    const uint32_t now = vkAddrCurrentFrame();
    constexpr uint32_t kFrames = 4;
    std::vector<const AsInputRec*> recs;
    const uint64_t have = std::min<uint64_t>(sAsInputSeq, kAsInputRing);
    for (uint64_t k = 0; k < have; ++k)
    {
        const AsInputRec& r = sAsInputs[(sAsInputSeq - 1 - k) % kAsInputRing];
        if (r.frame + kFrames < now) break;   // newest first: older frames follow
        recs.push_back(&r);
    }
    if (recs.empty()) { vkFaultLog("[vktest] AS build inputs: none in the last %u frames\n", kFrames); return; }
    std::vector<VkDeviceAddress> a(recs.size() * 2);
    for (size_t i = 0; i < recs.size(); ++i)
    {
        a[2 * i] = recs[i]->addr;
        a[2 * i + 1] = recs[i]->addr + (recs[i]->extent ? recs[i]->extent - 1 : 0);
    }
    std::vector<VkAddrHit> h(a.size());
    if (!vkAddrClassify(a.data(), a.size(), h.data())) { vkFaultLog("[vktest] AS build inputs: registry busy\n"); return; }
    uint32_t bad = 0, printed = 0, asyncN = 0;
    for (size_t i = 0; i < recs.size(); ++i)
    {
        const AsInputRec& r = *recs[i];
        if (r.async) ++asyncN;
        const VkAddrHit& s0 = h[2 * i];
        const VkAddrHit& s1 = h[2 * i + 1];
        const bool liveStart = s0.cls == VKADDR_BUF_LIVE || s0.cls == VKADDR_AS_LIVE;
        const bool sameRange = s0.lo == s1.lo && s0.cls == s1.cls;
        const bool ok = r.addr >= (1ull << 20) && liveStart && sameRange;
        if (ok) continue;
        ++bad;
        if (printed >= 32) continue;
        ++printed;
        static const char* const kCls[] = {"nothing known", "live AS", "previously freed AS range", "live buffer",
                                           "previously freed buffer range"};
        // #973: a live object reusing a freed range names that range too.
        char prevTxt[96] = "";
        if (s0.prevKind)
            std::snprintf(prevTxt, sizeof(prevTxt), " (reuses a previously freed %s range '%s' freed frame %u)",
                          s0.prevKind == 1 ? "AS" : "buffer", s0.prevName, s0.prevFreedFrame);
        vkFaultLog("[vktest]   AS build input %s%s%s frame %u%s: 0x%llx +%llu -> start in %s '%s' [0x%llx +%llu]%s%s%s, "
                   "end in %s\n",
                   kAsInputName[r.kind % 6], r.tlas ? " (TLAS)" : "", r.update ? " (refit)" : "", r.frame,
                   r.async ? " [async queue]" : "", (unsigned long long)r.addr, (unsigned long long)r.extent,
                   kCls[s0.cls % 5], s0.name, (unsigned long long)s0.lo, (unsigned long long)s0.size,
                   s0.freedFrame ? " freed frame " : "", s0.freedFrame ? std::to_string(s0.freedFrame).c_str() : "",
                   prevTxt, sameRange ? "the same range" : kCls[s1.cls % 5]);
    }
    vkFaultLog("[vktest] AS build inputs: %zu checked over registry frames %u..%u (%u recorded on the async queue): "
               "%u outside a live buffer / AS, below 1 MB, or running past their buffer's end\n",
               recs.size(), now >= kFrames ? now - kFrames : 0u, now, asyncN, bad);
}

uint32_t vkRecentMarks(const char** out, uint32_t max)
{
    if (!out || !max) return 0;
    const uint32_t end = sMarkSeq.load(std::memory_order_relaxed);
    const uint32_t n = std::min(std::min(end, MARK_RING), max);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char* nm = sMarkRing[(end - n + i) % MARK_RING].load(std::memory_order_relaxed);
        out[i] = nm ? nm : "?";
    }
    return n;
}

#undef vkQueueSubmit
#undef vkQueueSubmit2
#undef vkWaitForFences
#undef vkWaitSemaphores
#undef vkGetFenceStatus
#undef vkQueueWaitIdle
#undef vkGetSemaphoreCounterValue
#undef vkGetQueryPoolResults

static VkResult vkLostCheck(VkResult r, const char* call, const char* queue, const char* file, int line)
{
    if (r == VK_ERROR_DEVICE_LOST) vkDeviceLostRaise(call, r, queue, file, line);
    return r;
}

// Infinite waits run in slices (vk_common.h): warn once after the first slice, treat the
// wait as a GPU hang after kHangSlices. Finite / polling waits pass straight through.
static constexpr uint64_t kWaitSliceNs = 5000000000ull;   // 5 s
static constexpr uint32_t kHangSlices  = 12;              // 60 s
template <typename WaitFn>
static VkResult vkSlicedWait(WaitFn&& wait, uint64_t timeout, const char* call, const char* file, int line)
{
    if (timeout != UINT64_MAX) return vkLostCheck(wait(timeout), call, nullptr, file, line);
    for (uint32_t s = 1;; ++s)
    {
        const VkResult r = wait(kWaitSliceNs);
        if (r != VK_TIMEOUT) return vkLostCheck(r, call, nullptr, file, line);
        if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;   // raised elsewhere meanwhile
        if (s == 1)
        {
            vkFaultLog("[vkgfx] GPU STALL: %s at %s:%d has waited %u s for the GPU; recent GPU marks:",
                       call, file, line, (unsigned)(kWaitSliceNs / 1000000000ull));
            const char* marks[16];
            const uint32_t n = vkRecentMarks(marks, 16);
            for (uint32_t i = 0; i < n; ++i) vkFaultLog(" %s", marks[i]);
            vkFaultLog("\n");
            vkFaultFlushLine();
        }
        if (s >= kHangSlices)
        {
            vkDeviceLostRaise(call, VK_TIMEOUT, nullptr, file, line);   // GPU hang: no progress for 60 s
            return VK_ERROR_DEVICE_LOST;
        }
    }
}

VkResult vkqQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* si, VkFence f, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_SUBMIT))
        return vkLostCheck(VK_ERROR_DEVICE_LOST, "vkQueueSubmit (simulated)", vkQueueName(q), file, line);
    sSubmitSeq.fetch_add(1, std::memory_order_acq_rel);   // #699 (before the driver call: see vkqDeviceWaitIdle)
    vkSubmitLogNote(q, "vkQueueSubmit", file, line);       // #845 / #917
    return vkLostCheck(vkQueueSubmit(q, n, si, f), "vkQueueSubmit", vkQueueName(q), file, line);
}

VkResult vkqQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* si, VkFence f, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_SUBMIT))
        return vkLostCheck(VK_ERROR_DEVICE_LOST, "vkQueueSubmit2 (simulated)", vkQueueName(q), file, line);
    sSubmitSeq.fetch_add(1, std::memory_order_acq_rel);   // #699 (before the driver call: see vkqDeviceWaitIdle)
    vkSubmitLogNote(q, "vkQueueSubmit2", file, line);      // #845 / #917
    return vkLostCheck(gVkCompat.QueueSubmit2(q, n, si, f), "vkQueueSubmit2", vkQueueName(q), file, line);
}

VkResult vkqWaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout,
                          const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_WAIT))
        return vkLostCheck(VK_ERROR_DEVICE_LOST, "vkWaitForFences (simulated)", nullptr, file, line);
    const double t0 = timeout ? vkNowMs() : 0.0;
    const VkResult r = vkSlicedWait([&](uint64_t t) { return vkWaitForFences(d, n, f, all, t); }, timeout,
                                    "vkWaitForFences", file, line);
    if (timeout)
    {   // #699: blocking waits only (polls with timeout 0 are not waits)
        const double ms = vkNowMs() - t0;
        if (ms >= 0.5) vkWaitNote("fence", file, line, ms, false);
    }
    return r;
}

VkResult vkqWaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo* wi, uint64_t timeout, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    if (vkSimulateDeviceLostTake(VK_SIM_LOST_WAIT))
        return vkLostCheck(VK_ERROR_DEVICE_LOST, "vkWaitSemaphores (simulated)", nullptr, file, line);
    const double t0 = timeout ? vkNowMs() : 0.0;
    const VkResult r = vkSlicedWait([&](uint64_t t) { return vkWaitSemaphores(d, wi, t); }, timeout,
                                    "vkWaitSemaphores", file, line);
    if (timeout)
    {   // #699 (as vkqWaitForFences)
        const double ms = vkNowMs() - t0;
        if (ms >= 0.5) vkWaitNote("semaphore", file, line, ms, false);
    }
    return r;
}

VkResult vkqGetFenceStatus(VkDevice d, VkFence f, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    return vkLostCheck(vkGetFenceStatus(d, f), "vkGetFenceStatus", nullptr, file, line);
}

VkResult vkqQueueWaitIdle(VkQueue q, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    const double t0 = vkNowMs();
    const VkResult r = vkLostCheck(vkQueueWaitIdle(q), "vkQueueWaitIdle", vkQueueName(q), file, line);
    vkWaitNote("queueIdle", file, line, vkNowMs() - t0, false);   // #699
    return r;
}

VkResult vkqGetSemaphoreCounterValue(VkDevice d, VkSemaphore s, uint64_t* v, const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    return vkLostCheck(vkGetSemaphoreCounterValue(d, s, v), "vkGetSemaphoreCounterValue", nullptr, file, line);
}

VkResult vkqGetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                void* data, VkDeviceSize stride, VkQueryResultFlags flags,
                                const char* file, int line)
{
    if (vkDeviceIsLost()) return VK_ERROR_DEVICE_LOST;
    return vkLostCheck(vkGetQueryPoolResults(d, pool, first, count, size, data, stride, flags),
                       "vkGetQueryPoolResults", nullptr, file, line);
}

// ---- NULL PIPELINE GUARD (#672; vk_common.h routes every vkgfx caller here) ----
#undef vkCmdBindPipeline
#undef vkCmdDraw
#undef vkCmdDrawIndexed
#undef vkCmdDrawIndirect
#undef vkCmdDrawIndexedIndirect
#undef vkCmdDrawIndexedIndirectCount
#undef vkCmdDispatch
#undef vkCmdDispatchIndirect
namespace {
// Command buffers this thread bound a NULL pipeline into, per bind point (graphics /
// compute). Empty in the normal case: every hook then costs one thread-local size test.
struct NullBound
{
    VkCommandBuffer cmd;
    bool gfx;
    bool comp;
};
constexpr uint32_t kNullBoundMax = 8;
thread_local NullBound t_nullBound[kNullBoundMax];
thread_local uint32_t t_nullBoundN = 0;
std::atomic<uint32_t> sNullSkips{0};
std::atomic<bool> sNullBindLogged{false}, sNullSkipLogged{false};

NullBound* nullBoundFind(VkCommandBuffer cmd)
{
    for (uint32_t i = 0; i < t_nullBoundN; ++i)
        if (t_nullBound[i].cmd == cmd) return &t_nullBound[i];
    return nullptr;
}

void nullBoundSet(VkCommandBuffer cmd, VkPipelineBindPoint bp, bool null)
{
    const bool gfx = bp == VK_PIPELINE_BIND_POINT_GRAPHICS;
    NullBound* e = nullBoundFind(cmd);
    if (!e)
    {
        if (!null) return;
        if (t_nullBoundN == kNullBoundMax)   // oldest out (a long-finished recording)
        {
            for (uint32_t i = 1; i < kNullBoundMax; ++i) t_nullBound[i - 1] = t_nullBound[i];
            --t_nullBoundN;
        }
        e = &t_nullBound[t_nullBoundN++];
        *e = NullBound{cmd, false, false};
    }
    if (gfx) e->gfx = null;
    else e->comp = null;
    if (!e->gfx && !e->comp) *e = t_nullBound[--t_nullBoundN];   // (last entry moves into the hole)
}

// True: skip this draw (gfx) / dispatch (!gfx): the last pipeline bound there was NULL.
inline bool nullSkip(VkCommandBuffer cmd, bool gfx)
{
    if (!t_nullBoundN) return false;
    const NullBound* e = nullBoundFind(cmd);
    if (!e || !(gfx ? e->gfx : e->comp)) return false;
    sNullSkips.fetch_add(1, std::memory_order_relaxed);
    if (!sNullSkipLogged.exchange(true))
        std::fprintf(stderr, "[vkgfx] #672: skipping %s recorded after a NULL pipeline bind (a pipeline that"
                     " failed to compile; its feature is off). Logged once.\n", gfx ? "draws" : "dispatches");
    return true;
}
}

uint32_t vkNullPipelineSkips() { return sNullSkips.load(std::memory_order_relaxed); }

void vkqCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipeline p)
{
    if (p == VK_NULL_HANDLE)
    {
        nullBoundSet(cmd, bp, true);
        if (!sNullBindLogged.exchange(true))
            std::fprintf(stderr, "[vkgfx] #672: NULL %s pipeline bind skipped (a pipeline that failed to compile)."
                         " Logged once.\n", bp == VK_PIPELINE_BIND_POINT_GRAPHICS ? "graphics" : "compute");
        return;
    }
    if (t_nullBoundN) nullBoundSet(cmd, bp, false);
    vkCmdBindPipeline(cmd, bp, p);
}

void vkqCmdDraw(VkCommandBuffer cmd, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    if (nullSkip(cmd, true)) return;
    vkCmdDraw(cmd, vc, ic, fv, fi);
}

void vkqCmdDrawIndexed(VkCommandBuffer cmd, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{
    if (nullSkip(cmd, true)) return;
    vkCmdDrawIndexed(cmd, xc, ic, fx, vo, fi);
}

void vkqCmdDrawIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride)
{
    if (nullSkip(cmd, true)) return;
    vkCmdDrawIndirect(cmd, b, o, n, stride);
}

void vkqCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride)
{
    if (nullSkip(cmd, true)) return;
    vkCmdDrawIndexedIndirect(cmd, b, o, n, stride);
}

void vkqCmdDrawIndexedIndirectCount(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, VkBuffer cb, VkDeviceSize co,
                                    uint32_t maxN, uint32_t stride)
{
    if (nullSkip(cmd, true)) return;
    vkCmdDrawIndexedIndirectCount(cmd, b, o, cb, co, maxN, stride);
}

void vkqCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
    if (nullSkip(cmd, false)) return;
    vkCmdDispatch(cmd, x, y, z);
}

void vkqCmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o)
{
    if (nullSkip(cmd, false)) return;
    vkCmdDispatchIndirect(cmd, b, o);
}
