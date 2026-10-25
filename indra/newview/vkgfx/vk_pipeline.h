// vktest — pipeline + shader helpers (dynamic rendering, no VkRenderPass).
#pragma once

#include "vk_device.h"

#include <cstdint>
#include <string>
#include <vector>

// Loads a .spv file (compiled at build time by glslc) into a shader module.
VkShaderModule loadShaderModule(VkDevice device, const std::string& spvPath);

// A small builder for a graphics pipeline that renders with dynamic rendering
// (VK_KHR_dynamic_rendering). Keeps the common defaults so scenes stay short.
struct GraphicsPipelineDesc {
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    // MESH-SHADER pipeline (VK_EXT_mesh_shader; the cluster LOD mesh tier, #745): `mesh`
    // set replaces the vertex stage (vert, bindings / attributes and the input assembly
    // are then ignored); `task` is the optional amplification stage before it. Only
    // valid where the device enabled the mesh shader feature (LLCleanClusterCaps).
    VkShaderModule task = VK_NULL_HANDLE;
    VkShaderModule mesh = VK_NULL_HANDLE;

    // Vertex input (optional — empty means no vertex buffers, e.g. fullscreen).
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;

    VkPipelineLayout layout = VK_NULL_HANDLE;

    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // UNDEFINED = no depth
    bool depthTest = false;
    bool depthWrite = false;
    // Standard src-alpha / one-minus-src-alpha over blend on the single color
    // attachment (for the alpha-blend bucket). Default off (opaque write).
    bool blend = false;
    // Additive blend (ONE, ONE) on the color attachment -- post composites (glow).
    // Takes precedence over `blend`.
    bool additive = false;
    // Optional SECOND color attachment (the glow target). UNDEFINED = single
    // attachment. It blends like attachment 0; glowWrite=false masks it off (for
    // pipelines drawn inside the glow-carrying passes that emit no glow: sky, water).
    VkFormat glowFormat = VK_FORMAT_UNDEFINED;
    bool glowWrite = true;
    // Optional THIRD color attachment (F5: render pass 2's reactive / water mask, RG8;
    // needs the glow attachment). Blend MAX (ONE, ONE) so overlapping writers keep the
    // largest value; reactWriteMask picks the channels this pipeline writes (R reactive
    // for the post-water alpha / particles, G the TAA water mask for the water, 0 for
    // pass-2 pipelines that write neither -- the attachment stays declared so the
    // pipeline matches the pass's dynamic-rendering formats).
    VkFormat reactFormat = VK_FORMAT_UNDEFINED;
    VkColorComponentFlags reactWriteMask = 0;
    // PLAIN MRT mode (the deferred G-buffer pass, RenderVKDeferredLighting): when
    // mrtCount > 0 it REPLACES the colour / glow / react attachments above with
    // mrtCount (<= kMaxMrt) attachments of mrtFormats, each written in full with
    // blending off. colorWrite = false still masks them all.
    static constexpr uint32_t kMaxMrt = 4;
    uint32_t mrtCount = 0;
    VkFormat mrtFormats[kMaxMrt] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED,
                                    VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    // Wireframe / overlay knobs (defaults = the world pipelines' state).
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCompareOp depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    float depthBiasConstant = 0.f;   // != 0 enables depth bias
    float depthBiasSlope = 0.f;
    float lineWidth = 1.f;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // Depth-only pipelines (shadow maps): no color attachment; frag may then be
    // VK_NULL_HANDLE (vertex-only). depthClamp needs the device feature.
    bool noColor = false;
    bool depthClamp = false;
    // Explicit colour blend factors on attachment 0 (SL particle blend buckets); the
    // destination alpha is preserved. Takes precedence over `additive` / `blend`.
    bool customBlend = false;
    VkBlendFactor srcColor = VK_BLEND_FACTOR_ONE, dstColor = VK_BLEND_FACTOR_ZERO;
    // With customBlend: attachment 0's ALPHA accumulates the destination's transmittance
    // instead of being preserved -- srcAlpha ZERO, dstAlpha = dstColor (the reduced-
    // resolution particle accumulator: dst' = dst * T + C over a (0, 0, 0, 1) clear).
    bool partAccum = false;
    // Glow attachment (1) blends additively (ONE, ONE) whatever attachment 0 does.
    bool glowAdditive = false;
    // false: colorWriteMask 0 on EVERY colour attachment (attachments stay declared so
    // the pipeline matches the pass's dynamic-rendering formats; e.g. occlusion proxies).
    bool colorWrite = true;
    // Optional specialization constants for the fragment stage (e.g. the world FS's
    // overdraw-stats switch). Must outlive the createGraphicsPipeline call.
    const VkSpecializationInfo* fragSpec = nullptr;
    // Cull mode, front face and depth test / write / compare become DYNAMIC state (core
    // Vulkan 1.3): pipelines that differ only in those then share one compile (the
    // batch groups them and builds the rest from a pipeline cache). The values above
    // are recorded per handle and set by bindGraphicsPipeline(), which must be used to
    // bind such a pipeline.
    bool dynamicDepthCull = false;
    // Fragment shading rate is DYNAMIC (VK_KHR_fragment_shading_rate must be enabled):
    // the recorder sets it (VkDeviceContext::cmdSetShadingRate) before drawing.
    bool dynamicShadingRate = false;
    // The pipeline may be bound inside a rendering scope that carries a fragment
    // shading-rate ATTACHMENT (VRS tier 2; VUID-vkCmdDraw-imageView-06183): sets
    // VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR. Harmless in
    // scopes without one. Without dynamicShadingRate the attachment is ignored (KEEP).
    bool fsrAttachment = false;
    // Main-view pipeline under REVERSED-Z (near 1, far 0): createGraphicsPipeline flips
    // depthCompare (LESS <-> GREATER, LEQUAL <-> GEQUAL) and the depth-bias sign. Write
    // the desc in conventional terms; shadow / probe / mirror / HUD pipelines leave it off.
    bool reverseZ = false;
};

// Binds a graphics pipeline and, for a dynamicDepthCull one, sets the cull / front
// face / depth state it was created with. Use for every graphics bind.
void bindGraphicsPipeline(VkCommandBuffer cmd, VkPipeline pipeline);
// The pipeline takes cull / front face / depth state dynamically (registered above).
bool pipelineHasDynamicRaster(VkPipeline pipeline);
// Drops a pipeline's dynamic-raster record: call right before vkDestroyPipeline of a
// pipeline the session may create again (the setting specialization sets churn): the
// driver reuses handle values, and a stale record would make bindGraphicsPipeline set
// dynamic state on a later, static pipeline with the same handle. Bounded registry.
void vkForgetPipeline(VkPipeline pipeline);

VkPipeline createGraphicsPipeline(VkDevice device,
                                  const GraphicsPipelineDesc& desc);

// PARALLEL PIPELINE BATCH. A cold driver cache (any shader change: every new release)
// compiles each pipeline from scratch -- ~0.5 s for a world-FS variant, ~25-40 s for
// the renderer's init one after another on the main thread. Inside a
// PipelineBatchScope, createGraphicsPipelineInto() only queues (a deep copy of the
// desc, incl. its specialization info) and destroyShaderModuleDeferred() keeps the
// modules alive; pipelineBatchFlush() -- or the scope's end -- compiles the queue on
// all cores and writes each handle to its `out`. Read no queued handle before a
// flush (it is VK_NULL_HANDLE until then). Outside a scope both act immediately.
// `name` (optional, must outlive the call only): how the batch report names this
// pipeline (#471), e.g. the member it is written to; the shader module names
// (vkNoteShaderModule) are appended.
void createGraphicsPipelineInto(VkDevice device, const GraphicsPipelineDesc& desc, VkPipeline* out,
                                const char* name = nullptr);
// Shader module identity for the batch report and the learned compile costs (#471):
// the content hash of its SPIR-V (stable across launches, unlike the handle), its size
// and a short name. Call right after vkCreateShaderModule; a queued pipeline copies its
// modules' identity at createGraphicsPipelineInto time. Bounded registry; handles that
// were never noted get an anonymous name and contribute no hash to the learned key.
void vkNoteShaderModule(VkShaderModule module, const uint32_t* code, size_t bytes, const char* name);
void destroyShaderModuleDeferred(VkDevice device, VkShaderModule module,
                                 const VkAllocationCallbacks* alloc = nullptr);
void pipelineBatchFlush(VkDevice device);
// Asynchronous flush: hands the queue (and the deferred module destroys) to background
// threads and returns at once; nullptr if nothing was queued. Poll with
// pipelineJobDone(); pipelineJobFinish() waits, destroys the modules and frees the job
// (the queued `out` handles are all written once it returns).
struct PipelineJob;
PipelineJob* pipelineBatchFlushAsync(VkDevice device);
// The same into `cache` instead of the process-wide vkSharedPipelineCache() (the setting
// specialization sets compile into one VkPipelineCache per configuration key, so their
// binaries are persisted -- and evicted -- per key instead of growing the shared file;
// llcleanspecvk.cpp). `cache` must outlive the job (pipelineJobFinish).
PipelineJob* pipelineBatchFlushAsync(VkDevice device, VkPipelineCache cache);
// PRIORITY + PROCESS-WIDE CONCURRENCY. Every batch's compile workers share one limit:
// the compiles in flight across ALL batches stay within the compile thread count
// (hardware threads - 1, or RenderVKPipelineCompileThreads) -- two concurrent batches
// used to run "55 at once" on a 48-thread machine, each slowing the other -- and a
// worker of a LOWER priority waits while a batch of a higher priority still has
// pipelines nobody has taken. Compiles already running are never preempted (a cold
// RT lit FS takes ~16 s), so a prewarm batch also runs with few threads. Order: a
// pipeline the user is waiting for (a settings change) > login / first-use warm-up >
// speculative prewarm. Synchronous creates (createGraphicsPipeline / createComputePipeline
// on the render thread) are never held back by this.
enum VkPipePriority : uint8_t
{
    VKPIPE_PRIO_USER = 0,      // the user is looking at a fallback until it lands
    VKPIPE_PRIO_LOGIN = 1,     // world / first-use pipelines (the default)
    VKPIPE_PRIO_PREWARM = 2,   // speculative (a key the user may switch to)
    VKPIPE_PRIO_COUNT = 3
};
struct PipelineJobOpts
{
    VkPipelineCache cache = VK_NULL_HANDLE;   // VK_NULL_HANDLE = vkSharedPipelineCache()
    uint8_t prio = VKPIPE_PRIO_LOGIN;
    unsigned maxThreads = 0;                  // this job's worker cap (0 = the compile thread count)
    bool lowThreadPriority = false;           // its workers run below normal OS priority (Linux: nice +10)
    // Counts toward the SHADER GATE (vkPipelineGateProgress): the login progress screen
    // holds until every gate job compiled. Speculative / optional jobs pass false.
    bool gate = true;
    const char* what = nullptr;               // batch report label (static string)
};
PipelineJob* pipelineBatchFlushAsync(VkDevice device, const PipelineJobOpts& opts);
// ADMITTED SYNCHRONOUS CREATE: one pipeline compiled under the same process-wide memory
// budget, #672 concurrency cap / out-of-memory requeue and priority order as the batches,
// blocking the CALLING thread until it is admitted and done. For background workers
// (std::async) that used to call createComputePipeline / createGraphicsPipeline directly
// and so compiled on top of a batch that already filled the budget. Never on the render
// thread (it may wait behind other batches). VK_NULL_HANDLE on failure.
VkPipeline createComputePipelineAdmitted(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                                         VkPipelineShaderStageCreateFlags stageFlags,
                                         const VkSpecializationInfo* spec, const char* name,
                                         uint8_t prio = VKPIPE_PRIO_USER);
VkPipeline createGraphicsPipelineAdmitted(VkDevice device, const GraphicsPipelineDesc& desc, const char* name,
                                          uint8_t prio = VKPIPE_PRIO_USER);
// The SHADER GATE's progress: pipelines queued to / finished by gate jobs since launch,
// and gate jobs whose compile has not finished (a deferred job counts from
// pipelineBatchTakeDeferred until it finished or was dropped).
void vkPipelineGateProgress(unsigned& done, unsigned& queued, unsigned& jobsLive);
// COMPUTE pipelines in the batch: inside a PipelineBatchScope this only queues (the spec
// info is copied; `comp` must go to destroyShaderModuleDeferred, not vkDestroyShaderModule),
// and the batch compiles it with the graphics ones -- same scheduler, report, learned
// costs and out-of-memory requeue. Outside a scope it is createComputePipeline. Read the
// handle only once the batch / job finished (VK_NULL_HANDLE until then, and after a failure).
void createComputePipelineInto(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                               VkPipelineShaderStageCreateFlags stageFlags, const VkSpecializationInfo* spec,
                               VkPipeline* out, const char* name = nullptr);
// A lazily built pipeline group's background job (the first-use effects: god rays, motion
// blur, AA, ...): the frame never blocks on a cold compile. The owner queues its pipelines
// under a PipelineBatchScope and hands pipelineBatchFlushAsync's job to start(); poll()
// returns true ONCE, on the call that finds it finished (the outputs are written and the
// deferred modules destroyed then), so the owner checks what it got exactly there. Until
// then the effect is skipped (or falls back) -- never drawn with a half-built set.
// wait(): shutdown / rebuild (blocks until the compile finished).
struct PipelineJobSlot
{
    PipelineJob* job = nullptr;
    bool pending() const { return job != nullptr; }
    void start(PipelineJob* j) { wait(); job = j; }
    bool poll();
    void wait();
    ~PipelineJobSlot() { wait(); }
    PipelineJobSlot() = default;
    PipelineJobSlot(const PipelineJobSlot&) = delete;
    PipelineJobSlot& operator=(const PipelineJobSlot&) = delete;
};
// HARD CAP on the compile worker threads (0 = auto: hardware threads - 1). Memory is
// governed by the budget below (#471), not by this; it is the user's override
// (RenderVKPipelineCompileThreads).
void vkSetPipelineCompileThreads(unsigned n);
unsigned vkPipelineCompileThreads();
// Called from a compile worker every `every` completed compiles (serialized), so the
// app can persist the pipeline cache incrementally: a crash mid-batch keeps what was
// compiled instead of recompiling everything on the next launch.
void vkSetPipelineProgressHook(void (*fn)(), unsigned every);
// #672: also call it after a completed compile when `ms` passed since the last call (0 =
// by count only), and before an out-of-memory retry (so a crash in the driver afterwards
// still keeps everything compiled so far).
void vkSetPipelineProgressInterval(unsigned ms);
// Pipelines queued to / finished by every batch since launch, and seconds since the last
// one finished (0 before any batch). For the login progress screen.
void vkPipelineCompileProgress(unsigned& done, unsigned& queued, double& idle_sec);

// ---- Per-pipeline measurement (#471) ----
// The process's committed memory right now (Windows: private bytes, the commit charge
// a driver's compiler eats; elsewhere: resident set). Any thread; must be cheap (it is
// sampled every ~15 ms while a batch compiles). nullptr = no memory measurement.
void vkSetPipelineMemProbe(uint64_t (*fn)());
// One compiled pipeline of a batch. With concurrent compiles a raw delta also contains
// the neighbours' growth, so memory is ATTRIBUTED: every sample interval's change in
// committed bytes is split equally between the compiles in flight during it; `attribBytes`
// is the peak of that running share (what the learned cost uses), `rawBytes` the peak
// committed while it ran minus the value at its start, `concurrency` the most compiles
// in flight at once while it ran (itself included).
struct VkPipelineCompileStat
{
    std::string name;
    double ms = 0.0;
    uint64_t rawBytes = 0;
    uint64_t attribBytes = 0;
    uint64_t estBytes = 0;        // what the scheduler assumed when it admitted it
    uint32_t spirvWords = 0;      // vert + frag SPIR-V size (cost-proxy check)
    unsigned concurrency = 0;
    bool rest = false;            // a dedup twin built from the cache after the first group
    bool hit = false;             // fast enough to be a pipeline-cache hit
    unsigned attempts = 0;        // #672: compile attempts (> 1 = retried after out-of-memory)
    bool failed = false;          // #672: given up (VK_NULL_HANDLE)
};
struct VkPipelineBatchReport
{
    const char* what = "";
    size_t total = 0, compiled = 0, fromCache = 0;
    double wallMs = 0.0, restMs = 0.0, sumMs = 0.0;
    unsigned threads = 0;          // worker threads used
    unsigned maxConcurrency = 0;   // most compiles in flight at once
    uint64_t baseBytes = 0;        // committed bytes at the batch start (0 = no probe)
    uint64_t peakBytes = 0;        // highest committed bytes seen during the batch
    uint64_t budgetMin = UINT64_MAX; // lowest compile budget seen (UINT64_MAX = unlimited / no hook)
    unsigned overBudget = 0;       // compiles started over budget (progress guarantee)
    double waitMs = 0.0;           // worker time spent waiting for budget
    unsigned oomRetries = 0;       // #672: out-of-memory results requeued
    unsigned failed = 0;           // #672: pipelines given up (handle left VK_NULL_HANDLE)
    unsigned capEnd = 0;           // #672: concurrency cap at the end (0 = none)
    std::vector<VkPipelineCompileStat> items;
};
// Called once at the end of every batch (from the compiling thread; the async batch
// runs on a background thread). nullptr = a short stderr summary only.
void vkSetPipelineBatchHook(void (*fn)(const VkPipelineBatchReport&));

// ---- Memory-aware scheduling (#471) ----
// Some drivers' compilers need GBs per large pipeline (AMD's Windows driver: #458/#462,
// 23 parallel compiles ran a 32 GB machine out of commit at the login screen). Every
// queued pipeline carries an ESTIMATED memory cost: its learned cost (below) when its
// key is known, else the default. Workers take the most expensive pending pipeline
// (longest first) whose estimate fits, cheaper ones fill the remaining room:
//     sum over in-flight compiles of max(0, estimate - growth attributed so far)
//   + this estimate  <=  budget()
// (budget() is LIVE headroom, which the in-flight compiles' growth already reduced, so
// only what they may still allocate counts). A cheaper pipeline jumps the queue only if
// the head's estimate still fits on top of it (or the head can never fit the budget),
// so fillers cannot starve the head. When nothing at all is in flight the head
// starts regardless (progress guarantee; counted and logged). Waiting workers re-check
// every 50 ms or when a compile finishes.
// Budget hook: bytes the app allows compiles to use now (0 = pause: one at a time).
// nullptr = unlimited (only the thread count limits). Any thread; keep it cheap.
void vkSetPipelineMemBudget(uint64_t (*fn)());
// Cap on the compiles in flight (every batch): the sum of max(estimate, growth attributed
// so far) plus the next estimate stays <= cap, whatever the live headroom says -- that
// headroom is "all available memory less the reserve", which let a 16 GB machine's driver
// compilers take ~10 GB at the login screen and the world load after it hit the OOM
// killer (Arc B580 / Mesa). The progress guarantee still starts one compile when nothing
// is in flight. 0 = no cap. Any thread.
void vkSetPipelineMemCap(uint64_t bytes);
// Estimated cost of a pipeline with no learned cost (by driver; default 512 MB).
void vkSetPipelineDefaultCost(uint64_t bytes);
// The pipeline cache was loaded from disk this session: pipelines whose last compile was
// a cache hit are expected to hit again and get a tiny estimate. Without a warm cache
// their learned COLD cost applies (a hit says nothing about a cold compile).
void vkSetPipelineCacheWarm(bool warm);
// LEARNED COSTS, keyed by a launch-stable hash (shader SPIR-V hashes + every non-handle
// state field + specialization data). Per key: cold cost (max of the attributed peaks,
// older observations decayed x0.9 per new one), cold compile time, whether the last
// compile was a cache hit, the session it was last seen. Bounded (1024 keys; keys unseen
// for 8 sessions are dropped). Text, one entry per line; the caller persists it and
// checks the device / driver it belongs to. Import (once, before the first batch) starts
// a new session and returns the entries accepted (malformed lines are skipped); export
// (any thread) also folds in the attributed peaks of compiles still in flight, so a save
// during a batch that later crashes still teaches the next launch.
size_t vkPipelineCostImport(const std::string& text);
std::string vkPipelineCostExport();
bool pipelineJobDone(const PipelineJob* job);
// Waits for a started job; an unstarted one (pipelineBatchTakeDeferred) is DROPPED: its
// `out` handles stay VK_NULL_HANDLE. Either way its modules are destroyed.
void pipelineJobFinish(PipelineJob* job);
struct PipelineBatchScope
{
    explicit PipelineBatchScope(VkDevice device);
    ~PipelineBatchScope();
    PipelineBatchScope(const PipelineBatchScope&) = delete;
    PipelineBatchScope& operator=(const PipelineBatchScope&) = delete;
    VkDevice mDevice;
};
// DEFERRED GROUP (cold-start ordering): inside a PipelineBatchScope, pipelines queued
// while one of these is alive go to a separate group that the scope's flushes leave
// alone -- pipelines the first frames do not need (e.g. the ray-query lit variants,
// which have a plain fallback). pipelineBatchTakeDeferred() takes the group as a job
// WITHOUT starting it; pipelineJobStart() compiles it later on background threads, through
// the same scheduler (memory budget, learned costs, out-of-memory requeue) as every batch.
// The shader modules its pipelines reference -- including ones the main group's
// pipelines share -- are destroyed with IT, so start it only once the main group's job
// has finished (pipelineJobFinish), and finish (or drop) it before the device goes.
// Read none of its `out` handles before pipelineJobFinish (workers write them).
struct PipelineBatchDeferScope
{
    PipelineBatchDeferScope();
    ~PipelineBatchDeferScope();
    PipelineBatchDeferScope(const PipelineBatchDeferScope&) = delete;
    PipelineBatchDeferScope& operator=(const PipelineBatchDeferScope&) = delete;
};
// The deferred group as an unstarted job (nullptr if empty). Call inside the scope,
// before its flush and after every module the group uses went to
// destroyShaderModuleDeferred (it takes those modules from the main group).
PipelineJob* pipelineBatchTakeDeferred(VkDevice device);
// Starts a job from pipelineBatchTakeDeferred (no-op if started / null). Re-arms the
// cold-start concurrency cap first (vkPipelineConcurrencyRearm).
void pipelineJobStart(PipelineJob* job);

// ---- PIPELINE EXECUTABLE STATISTICS (VK_KHR_pipeline_executable_properties) ----
// RenderVKPipelineStats: the device enabled pipelineExecutableInfo and handed its entry
// points here (VkDeviceContext, at device creation). While set, every graphics / compute
// pipeline created through this file (sync, batch, compute) gets
// VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR, and right after creation one log line per
// executable (shader stage) lists every statistic the driver reports (registers, spills,
// instructions, ... -- NVIDIA and AMD report different sets), its subgroup size and the
// pipeline's name + shader modules. The flag can slow creation and bypass the pipeline
// cache on some drivers, and the batch then learns no compile costs (#471). Off (null
// functions, the default): nothing is added, nothing queried.
void vkSetPipelineStatsCapture(PFN_vkGetPipelineExecutablePropertiesKHR props,
                               PFN_vkGetPipelineExecutableStatisticsKHR stats);
bool vkPipelineStatsCapture();
// Logs the statistics of an existing pipeline (no-op unless capture is on). `name` labels it.
void vkLogPipelineStats(VkDevice device, VkPipeline pipeline, const char* name);

// A compute pipeline from a single compute shader module + layout.
VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp,
                                 VkPipelineLayout layout);
// The same with shader-stage create flags (e.g. VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
// the caller checks the device enabled the feature the flag needs).
VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp,
                                 VkPipelineLayout layout, VkPipelineShaderStageCreateFlags stageFlags);
// The same with specialization constants (nullptr = none; must outlive the call).
VkPipeline createComputePipeline(VkDevice device, VkShaderModule comp, VkPipelineLayout layout,
                                 VkPipelineShaderStageCreateFlags stageFlags, const VkSpecializationInfo* spec);

// ---- RECOVERABLE PIPELINE CREATION (#672) ----
// A driver's shader compiler can run out of memory (AMD's Windows driver returned
// VK_ERROR_OUT_OF_HOST_MEMORY mid cold-compile at ~15 GB working set on a 32 GB machine;
// every launch crashed). VK_ERROR_OUT_OF_HOST_MEMORY / VK_ERROR_OUT_OF_DEVICE_MEMORY from
// pipeline creation is never fatal:
//  * the batch (createGraphicsPipelineInto) requeues the pipeline, HALVES the scheduler's
//    concurrency cap (process-wide, floor 1) and retries it after a backoff; once it
//    failed again while compiling ALONE kPipeAloneRetries times it is given up: its
//    handle stays VK_NULL_HANDLE.
//  * a synchronous create (createGraphicsPipeline / createComputePipeline / the UI
//    pipelines / vkCreatePipelineResilient) halves the cap too, waits for the compiles
//    in flight to drain (bounded backoff) and retries; after the retries it returns
//    VK_NULL_HANDLE.
// Any other error keeps the fatal path (gVkFatalHandler / device lost).
// A VK_NULL_HANDLE pipeline is a disabled feature: callers test their handle before use
// (the pass / feature is skipped), and as a last line of defence the command hooks in
// vk_common.h (vkqCmdBindPipeline) skip binding a NULL pipeline and the draws /
// dispatches recorded after it on that command buffer (logged once).
bool vkPipelineResultIsOom(VkResult r);
// One vkCreate*Pipelines call for ONE pipeline (`call` writes it), with the recovery
// above. `what` names it in the log; expr/file/line: the fatal report for other errors.
using VkPipelineCreateFn = VkResult (*)(void* ctx, VkPipeline* out);
VkPipeline vkCreatePipelineResilient(const char* what, VkPipelineCreateFn call, void* ctx, const char* expr,
                                     const char* file, int line);
// CRASH BREADCRUMB (#782: a crash inside the driver's shader compiler, on a driver thread,
// named no pipeline). Every pipeline creation that goes through this file (batch workers,
// synchronous creates on any thread) holds a slot with its name for the duration of the
// vkCreate*Pipelines call; other compile paths (the FX runtime) take one with
// VkPipelineCompileCrumb. The crash handler prints them with
// vkPipelineCompilesInFlightText: plain static buffers, no lock, no allocation (safe in
// an exception filter; a name may be torn if it is replaced mid-read). Bounded: 64 slots
// (a compile beyond that is simply not listed).
struct VkPipelineCompileCrumb
{
    explicit VkPipelineCompileCrumb(const char* name);
    ~VkPipelineCompileCrumb();
    VkPipelineCompileCrumb(const VkPipelineCompileCrumb&) = delete;
    VkPipelineCompileCrumb& operator=(const VkPipelineCompileCrumb&) = delete;
    int slot = -1;
};
// "<n> compiling: name (12.3 s); name (0.4 s)" into out (always NUL-terminated); returns
// the length written. "none" when nothing compiles.
size_t vkPipelineCompilesInFlightText(char* out, size_t size);
// The pipeline the given OS thread is compiling right now (the thread that took a crash inside
// the driver's compiler), copied into out; false when that thread holds no crumb. Same rules as
// above: no lock, no allocation. Thread ids are the OS ids (GetCurrentThreadId / gettid).
bool vkPipelineCompileOnThread(uint64_t os_thread_id, char* out, size_t size);
// #876 / #890: a crash during pipeline compilation (on a compile worker, or inside a driver
// shader compiler module on any thread) is recorded for the next start so it compiles one
// pipeline at a time. The marker is one line "<unix time> <sessions left>\n" written by the
// crash handler. Set the path once at startup (UTF-8; converted here, so the crash path
// allocates nothing); vkPipelineCrashMarkerWrite is then safe in a signal / exception filter:
// static buffers and open/write/close (CreateFileW/WriteFile) only. Returns true if written.
void vkPipelineCrashMarkerSetPath(const char* utf8_path);
bool vkPipelineCrashMarkerWrite();
// The crashing OS thread's compile crumb, or a fault inside a driver compiler: true when this
// crash should arm the marker (the thread holds a compile crumb). The module-name test lives
// with the platform crash handler (it alone can name the module).
bool vkPipelineCrashOnCompileThread(uint64_t os_thread_id);
// The same for the calling thread (the Unix fatal-signal hook runs on the faulting thread).
bool vkPipelineCrashOnCurrentThread();
// Clean sessions to compile one at a time after such a crash, before stepping back to automatic.
constexpr int kVkPipelineCrashSafeSessions = 3;
// #886 / #887 / #782: the last pipelines whose vkCreate*Pipelines returned, newest first, as
// "name (12.3 s ago); ..." ("none" when nothing has been created). The NVIDIA driver also
// compiles on its OWN threads after the create returned (a crash there holds no crumb, and
// "in flight" says none), so the crash report lists the recent ones as the candidates.
// Same rules as above: plain static buffers, no lock, no allocation. Bounded: 24 entries.
size_t vkPipelineRecentCompilesText(char* out, size_t size);
// STRAGGLERS: once vkSetPipelineMainThread ran (the world is drawing), every synchronous
// create on the render thread that took 3 ms or more (a real compile, not a
// pipeline cache hit) logs one line "VKPipe: sync create <name> <ms>" through the event
// hook, so a pipeline that still compiles inside a frame is visible. Count of them:
unsigned vkPipelineSyncStragglers();
// #699: the MAIN (render) thread. A synchronous create on it never blocks for the multi-
// second retry ladder (up to ~3.5 s per pipeline, a crossing-window stall): it makes at
// most one more attempt after waiting <= 100 ms for the compiles in flight, then returns
// VK_NULL_HANDLE, and for kMainOomCooldown s after that a main-thread create that runs
// out of memory again returns at once. Its owner asks again later (lazily created
// effects retry on their next ensure) -- not disabled for the session. Other threads keep
// the full ladder. Called from the render thread once the world pipelines are in
// (LLCleanRendererVK::finalizePipelines): init-time creates keep the full ladder.
void vkSetPipelineMainThread();
// Main-thread synchronous creates that returned VK_NULL_HANDLE without the full retry
// ladder (#699; monotonic) -- the #700 diagnostics.
unsigned vkPipelineMainFastFails();
// Pipelines given up for good this session (monotonic; the viewer shows a one-time notice).
unsigned vkPipelineFailedCount();
// Out-of-memory results seen from pipeline creation this session (each one retried).
unsigned vkPipelineOomCount();
// The scheduler's current concurrency cap (0 = none: the thread count / budget decide).
unsigned vkPipelineConcurrencyCap();
// Starting cap (before the first batch; 0 = none), e.g. a cold pipeline cache on a driver
// whose compiler is known to run out of memory. Additive increase: +1 after `cap` real
// (cold) compiles while the budget holds cap + 1 compiles at the largest measured cost;
// lifted once it reaches the thread count. An out-of-memory result halves it and bounds
// the ramp below the concurrency that failed for the rest of the session; it also keeps
// the budget below the committed memory the driver failed at (less 10%).
void vkSetPipelineConcurrencyStart(unsigned cap);
// Re-applies that starting cap, LOWER-ONLY (never raises a cap an out-of-memory result
// set, never touches the ramp ceiling; no-op when no starting cap was set): a deferred
// batch of heavy pipelines (pipelineJobStart) starts conservative again on a cold cache.
void vkPipelineConcurrencyRearm();
// Testing aid (RenderVKDebugSimulatePipelineOOM): the next `k` pipeline creations return
// VK_ERROR_OUT_OF_HOST_MEMORY WITHOUT calling the driver. batchOnly: only creations on the
// batch compile workers count (the start-up arm: exercises the requeue path of the world
// warm-up without failing the login screen's synchronous UI pipelines); otherwise every
// creation (batch, synchronous, UI, compute). A simulated failure halves the cap like a
// real one but neither bounds the ramp nor sets the committed-memory budget ceiling.
void vkSimulatePipelineOom(unsigned k, bool batchOnly = false);
// Log sink for the recovery path (step-downs, retries, failures): `warn` = a warning.
// Any thread. nullptr = stderr only.
void vkSetPipelineEventHook(void (*fn)(bool warn, const char* line));
