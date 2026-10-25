/**
 * @file llfxruntimevk.cpp
 * @brief Slipstream's built-in ReShade FX effects runtime on the VK post chain (#544).
 *        See llfxruntimevk.h for the injection points, inputs and safety rules.
 */
#include "linden_common.h"

#include "llfxruntimevk.h"
#include "vk_fx_shaders_spv.h"
#include "vk_pipeline.h"   // VkPipelineCompileCrumb (#782 crash breadcrumb)

#include "llerror.h"
#include "llimage.h"
#include "llsd.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <random>
#include <thread>
#include "llthread.h"   // ll_set_current_thread_name (#736)

namespace
{
constexpr uint32_t kMaxTexEdge = 16384;
constexpr uint32_t kMaxMips = 16;
constexpr uint32_t kMaxPassesPerTech = 64;
constexpr uint32_t kRingMax = 2;
constexpr uint64_t kIdleFreeFrames = 600;   // unused size entries / perms are freed after this
// Compile watchdog (see llfxruntimevk.h, COMPILE WATCHDOG). Every installed SweetFX / prod80
// effect builds all its pipelines in < 0.1 s per pass on an RTX 5090 (slfxc --time-pipes):
// 30 s is two orders of magnitude of headroom for slower CPUs and heavy effects.
constexpr double kCompileTimeoutSec = 30.0;
constexpr double kSyncWaitSec = 30.0;          // snapshot-size compile, main thread waits at most this
constexpr int kShutdownWaitMs = 2000;          // detachDevice / destructor: then leave the worker

// Static labels for GPU breadcrumbs (vkBreadcrumb keeps the pointer): one per run-order slot.
#define FXM(n) "fx_tech_" #n
const char* const kTechMarks[64] = {
    FXM(00), FXM(01), FXM(02), FXM(03), FXM(04), FXM(05), FXM(06), FXM(07), FXM(08), FXM(09),
    FXM(10), FXM(11), FXM(12), FXM(13), FXM(14), FXM(15), FXM(16), FXM(17), FXM(18), FXM(19),
    FXM(20), FXM(21), FXM(22), FXM(23), FXM(24), FXM(25), FXM(26), FXM(27), FXM(28), FXM(29),
    FXM(30), FXM(31), FXM(32), FXM(33), FXM(34), FXM(35), FXM(36), FXM(37), FXM(38), FXM(39),
    FXM(40), FXM(41), FXM(42), FXM(43), FXM(44), FXM(45), FXM(46), FXM(47), FXM(48), FXM(49),
    FXM(50), FXM(51), FXM(52), FXM(53), FXM(54), FXM(55), FXM(56), FXM(57), FXM(58), FXM(59),
    FXM(60), FXM(61), FXM(62), FXM(63)};
#undef FXM

// Device-lost forensics: what the last live frame recorded (any thread reads).
std::mutex sLastMutex;
std::string sLastRecorded;

std::mutex sMaskMutex;
std::function<VkImageView()> sMaskProvider;

enum Semantic : uint8_t
{
    SEM_NONE = 0,   // the effect's own texture
    SEM_COLOR,
    SEM_DEPTH,
    SEM_NORMAL,
    SEM_MOTION,
    SEM_MASK,
    SEM_HDR,
    SEM_UNKNOWN,
};

// Shared input images (fx_resolve.comp outputs), index = semantic - SEM_DEPTH.
enum InputKind : int { IN_DEPTH = 0, IN_NORMAL, IN_MOTION, IN_MASK, IN_HDR, IN_COUNT };
const VkFormat kInputFormat[IN_COUNT] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                         VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT,
                                         VK_FORMAT_R16G16B16A16_SFLOAT};

Semantic semanticOf(const std::string& s)
{
    if (s.empty()) return SEM_NONE;
    std::string u = s;
    std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    if (u == "COLOR" || u == "SV_TARGET") return SEM_COLOR;
    if (u == "DEPTH") return SEM_DEPTH;
    if (u == "SLIPSTREAM_NORMAL") return SEM_NORMAL;
    if (u == "SLIPSTREAM_MOTION") return SEM_MOTION;
    if (u == "SLIPSTREAM_AVATAR_MASK") return SEM_MASK;
    if (u == "SLIPSTREAM_SCENE_HDR") return SEM_HDR;
    return SEM_UNKNOWN;
}

VkFormat fxFormat(reshadefx::texture_format f)
{
    using tf = reshadefx::texture_format;
    switch (f)
    {
    case tf::r8: return VK_FORMAT_R8_UNORM;
    case tf::r16f: return VK_FORMAT_R16_SFLOAT;
    case tf::r16: return VK_FORMAT_R16_UNORM;
    case tf::r32f: return VK_FORMAT_R32_SFLOAT;
    case tf::r32u: return VK_FORMAT_R32_UINT;
    case tf::r32i: return VK_FORMAT_R32_SINT;
    case tf::rg8: return VK_FORMAT_R8G8_UNORM;
    case tf::rg16f: return VK_FORMAT_R16G16_SFLOAT;
    case tf::rg16: return VK_FORMAT_R16G16_UNORM;
    case tf::rg32f: return VK_FORMAT_R32G32_SFLOAT;
    case tf::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
    case tf::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case tf::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
    case tf::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case tf::rgba32u: return VK_FORMAT_R32G32B32A32_UINT;
    case tf::rgba32i: return VK_FORMAT_R32G32B32A32_SINT;
    case tf::rgb10a2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case tf::rg11b10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

VkFormat srgbOf(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool isIntFormat(VkFormat f)
{
    return f == VK_FORMAT_R32_UINT || f == VK_FORMAT_R32_SINT || f == VK_FORMAT_R32G32B32A32_UINT
        || f == VK_FORMAT_R32G32B32A32_SINT || f == VK_FORMAT_R16_UINT;
}

VkBlendFactor blendFactor(reshadefx::blend_factor b)
{
    using bf = reshadefx::blend_factor;
    switch (b)
    {
    case bf::zero: return VK_BLEND_FACTOR_ZERO;
    case bf::one: return VK_BLEND_FACTOR_ONE;
    case bf::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
    case bf::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case bf::dest_color: return VK_BLEND_FACTOR_DST_COLOR;
    case bf::one_minus_dest_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case bf::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case bf::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case bf::dest_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case bf::one_minus_dest_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp blendOp(reshadefx::blend_op o)
{
    using bo = reshadefx::blend_op;
    switch (o)
    {
    case bo::add: return VK_BLEND_OP_ADD;
    case bo::subtract: return VK_BLEND_OP_SUBTRACT;
    case bo::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case bo::min: return VK_BLEND_OP_MIN;
    case bo::max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkStencilOp stencilOp(reshadefx::stencil_op o)
{
    using so = reshadefx::stencil_op;
    switch (o)
    {
    case so::zero: return VK_STENCIL_OP_ZERO;
    case so::keep: return VK_STENCIL_OP_KEEP;
    case so::replace: return VK_STENCIL_OP_REPLACE;
    case so::increment_saturate: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case so::decrement_saturate: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case so::invert: return VK_STENCIL_OP_INVERT;
    case so::increment: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case so::decrement: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkCompareOp compareOp(reshadefx::stencil_func f)
{
    using sf = reshadefx::stencil_func;
    switch (f)
    {
    case sf::never: return VK_COMPARE_OP_NEVER;
    case sf::less: return VK_COMPARE_OP_LESS;
    case sf::equal: return VK_COMPARE_OP_EQUAL;
    case sf::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case sf::greater: return VK_COMPARE_OP_GREATER;
    case sf::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
    case sf::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case sf::always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkPrimitiveTopology topology(reshadefx::primitive_topology t)
{
    using pt = reshadefx::primitive_topology;
    switch (t)
    {
    case pt::point_list: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case pt::line_list: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case pt::line_strip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case pt::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkSamplerAddressMode addressMode(reshadefx::texture_address_mode m)
{
    using am = reshadefx::texture_address_mode;
    switch (m)
    {
    case am::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case am::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case am::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

std::string vkErr(const char* what, VkResult r)
{
    return std::string("error: ") + what + " failed (" + vkResultString(r) + ")";
}

std::string fileName(const std::string& path)
{
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// One full execution + memory barrier: every FX image lives in GENERAL, so pass-to-pass
// ordering needs no layout changes (post passes on full-screen images; simple and safe).
void barrierAll(VkCommandBuffer cmd)
{
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.memoryBarrierCount = 1; d.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &d);
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
    b.oldLayout = from; b.newLayout = to;
    b.image = img; b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1; d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

struct SamplerKey
{
    uint8_t filter, au, av, aw;
    float lodBias, minLod, maxLod;
    bool operator==(const SamplerKey& o) const
    {
        return filter == o.filter && au == o.au && av == o.av && aw == o.aw && lodBias == o.lodBias
            && minLod == o.minLod && maxLod == o.maxLod;
    }
};
}

// ============================================================================
// Data structures
// ============================================================================
struct LLFxRuntimeVK::Perm
{
    struct PassVK
    {
        bool compute = false;
        VkDescriptorSetLayout dsl1 = VK_NULL_HANDLE, dsl2 = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
        VkDescriptorSet set1 = VK_NULL_HANDLE, set2 = VK_NULL_HANDLE;
        uint32_t rtCount = 0;
        int rt[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // texture index; -1 = back buffer
        bool toBackBuffer = false;
        bool srgbWrite = false;
        bool stencil = false;
        uint32_t vpW = 0, vpH = 0, vpZ = 1;
    };
    struct TechVK
    {
        std::vector<PassVK> passes;
        bool usesStencil = false;
        bool enabledInScreenshot = true;
        uint8_t point = POINT_DISPLAY;
    };
    struct TexVK
    {
        Semantic sem = SEM_NONE;
        GpuImage img;
        VkFormat fmt = VK_FORMAT_UNDEFINED, srgbFmt = VK_FORMAT_UNDEFINED;
        VkImageView srgbView = VK_NULL_HANDLE;          // all mips
        std::vector<VkImageView> mipViews;               // UNORM, one per level (RT / storage)
        VkImageView rtSrgb = VK_NULL_HANDLE;             // SRGB mip 0 (SRGBWriteEnable)
        uint32_t w = 1, h = 1, levels = 1;
        bool linearBlit = false;
        std::vector<uint8_t> pixels;                     // decoded `source` (RGBA8, w x h), until uploaded
    };

    std::string key, effect;
    uint32_t w = 0, h = 0;
    uint8_t point = POINT_DISPLAY;
    float farPlane = 256.f;
    VkFormat bbFormat = VK_FORMAT_UNDEFINED;
    // 0 queued, 1 compiling, 2 compiled (pipelines built), 3 live, -1 failed
    std::atomic<int> state{0};
    std::string errors;
    std::shared_ptr<const slfx::CompileResult> cr;
    bool metaOnly = false;
    std::string cfgKey;   // configKey() at queue time (definitions / point / folders)

    // Worker-built
    std::vector<VkShaderModule> modules;
    VkDescriptorSetLayout dsl0 = VK_NULL_HANDLE;
    std::vector<TechVK> techs;
    std::vector<TexVK> tex;

    // Main thread (live)
    std::vector<VkSampler> samplers;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    GpuBuffer ubo;
    uint32_t uboStride = 0;
    uint64_t vramBytes = 0;
    uint64_t lastUsed = 0;
    uint64_t uboFrame = ~0ull;
    uint32_t goodFrames = 0;
    std::vector<uint8_t> uboData;
    // uniform special state (by uniform index)
    std::vector<float> pingpong;   // value, direction pairs
    std::vector<uint8_t> keyToggle, keyPrev;
};

struct LLFxRuntimeVK::Shared
{
    uint32_t w = 0, h = 0;
    uint8_t point = POINT_DISPLAY;
    VkFormat bbFormat = VK_FORMAT_UNDEFINED, bbSrgb = VK_FORMAT_UNDEFINED;
    GpuImage bbRT, bbTex;
    VkImageView bbRTSrgb = VK_NULL_HANDLE, bbTexSrgb = VK_NULL_HANDLE;
    GpuImage input[IN_COUNT];
    GpuImage scopeSave;
    GpuImage stencil;
    VkFormat stencilFmt = VK_FORMAT_UNDEFINED;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet blitSet = VK_NULL_HANDLE;
    VkDescriptorSet scopeSet = VK_NULL_HANDLE;
    VkDescriptorSet resolveSet[IN_COUNT][kRingMax] = {};
    VkDescriptorSet hdrInSet[kRingMax] = {};
    VkDescriptorSet hdrOutSet[kRingMax] = {};
    bool scopeSetWritten = false;
    uint64_t lastUsed = 0;
    uint64_t resolvedFrame = ~0ull;
    uint32_t resolvedMask = 0;
    uint64_t vramBytes = 0;
};

struct LLFxRuntimeVK::Host
{
    VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
    std::vector<std::pair<SamplerKey, VkSampler>> samplerCache;
    VkDescriptorSetLayout blitDsl = VK_NULL_HANDLE;      // 0 CIS
    VkDescriptorSetLayout scopeDsl = VK_NULL_HANDLE;     // 0..2 CIS
    VkDescriptorSetLayout resolveDsl = VK_NULL_HANDLE;   // 0,1 CIS, 2,3 storage
    VkDescriptorSetLayout hdrDsl = VK_NULL_HANDLE;       // 0 CIS, 1 storage
    VkPipelineLayout blitLayout = VK_NULL_HANDLE, scopeLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE, hdrLayout = VK_NULL_HANDLE;
    VkPipeline resolve[IN_COUNT] = {};
    std::vector<std::pair<VkFormat, VkPipeline>> blitPipes, scopePipes, hdrInPipes, hdrOutPipes;
    VkShaderModule vs = VK_NULL_HANDLE, blitFs = VK_NULL_HANDLE, scopeFs = VK_NULL_HANDLE;
    VkShaderModule hdrInFs = VK_NULL_HANDLE, hdrOutFs = VK_NULL_HANDLE;
    GpuImage dummyColor;     // 1x1 RGBA8 zero (unknown semantics), GENERAL
    GpuImage dummyR32F;      // 1x1 R32F (exposure fallback), GENERAL, storage
};

// ============================================================================
// Compile worker: preprocess + parse + SPIR-V + modules + layouts + pipelines +
// texture file decode, one job at a time, off the main thread.
// ============================================================================
struct LLFxRuntimeVK::Job
{
    std::shared_ptr<Perm> perm;
    VkPipelineCache cache = VK_NULL_HANDLE;   // the runtime's private user-effect cache
    const std::atomic<bool>* abandon = nullptr;   // the worker's flag: stop at the next check point
    std::atomic<int> stage{0};                 // 0 front end, 1 inside the driver (modules / pipelines)
    slfx::CompileRequest req;
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceLimits limits{};
    bool robust = false;   // VK_EXT_pipeline_robustness enabled (#546)
    VkPipelineRobustnessBufferBehaviorEXT robustBuffer = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
    VkPipelineRobustnessImageBehaviorEXT robustImage = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DEVICE_DEFAULT_EXT;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;   // for stencil passes (the worker has no ctx)
    std::vector<std::string> textureDirs;
};

namespace
{
bool loadSourceTexture(const std::vector<std::string>& dirs, const std::string& name, uint32_t w, uint32_t h,
                       std::vector<uint8_t>& out, std::string& err)
{
    std::string path;
    for (const std::string& d : dirs)
    {
        std::string p = d;
        if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
        p += name;
        if (LLFile::isfile(p)) { path = p; break; }
    }
    if (path.empty()) { err = "warning: texture source '" + name + "' not found in the Textures folders"; return false; }
    LLPointer<LLImageFormatted> img = LLImageFormatted::createFromExtension(path);
    if (img.isNull()) { err = "warning: texture source '" + name + "': unsupported file type (png, jpg, bmp, tga)"; return false; }
    if (!img->load(path)) { err = "warning: texture source '" + name + "' could not be read"; return false; }
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!img->decode(raw, 0.f) || !raw->getData()) { err = "warning: texture source '" + name + "' could not be decoded"; return false; }
    if ((uint32_t)raw->getWidth() != w || (uint32_t)raw->getHeight() != h) raw->scale((S32)w, (S32)h, true);
    const S32 c = raw->getComponents();
    if (c < 1 || c > 4 || (uint32_t)raw->getWidth() != w || (uint32_t)raw->getHeight() != h)
    { err = "warning: texture source '" + name + "' has an unsupported layout"; return false; }
    out.resize((size_t)w * h * 4);
    const U8* src = raw->getData();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
    {
        const U8* s = src + i * c;
        U8* d = &out[i * 4];
        if (c == 1) { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
        else if (c == 2) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; }
        else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = c == 4 ? s[3] : 255; }
    }
    return true;
}

// Build modules, layouts and pipelines for a compiled module. No VK_CHECK: every failure
// is reported and leaves the perm failed (the caller destroys what was created).
bool buildPipelines(LLFxRuntimeVK::Job& job, std::string& err);
}

struct LLFxRuntimeVK::Worker
{
    std::mutex m;
    std::condition_variable cv;       // queue / quit
    std::condition_variable idleCv;   // a job finished, or the thread exited
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::shared_ptr<Perm>> done;
    bool quit = false;
    bool busy = false;
    bool exited = false;
    std::shared_ptr<Job> current;                      // in flight (under m)
    std::chrono::steady_clock::time_point busySince;   // when `current` started (under m)
    std::atomic<bool> abandon{false};   // results unwanted: stop at the next check point
    std::thread thread;

    Worker() { thread = std::thread([this] { run(); }); }
    // Only for a worker that is known to have exited or to be idle: see stop().
    ~Worker()
    {
        stop();
        if (thread.joinable()) thread.join();
    }
    void stop()
    {
        { std::lock_guard<std::mutex> lk(m); quit = true; queue.clear(); }
        abandon = true;
        cv.notify_all();
    }
    void push(const std::shared_ptr<Job>& j, bool front = false)
    {
        j->abandon = &abandon;
        {
            std::lock_guard<std::mutex> lk(m);
            if (front) queue.push_front(j); else queue.push_back(j);
        }
        cv.notify_one();
    }
    // Waits until no job is in flight (or the thread exited), at most `ms`. true = idle.
    bool waitIdle(int ms)
    {
        std::unique_lock<std::mutex> lk(m);
        return idleCv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !busy || exited; });
    }
    static void execute(Job& j)
    {
        Perm& p = *j.perm;
        p.state = 1;
        auto res = std::make_shared<slfx::CompileResult>();
        slfx::compile(j.req, *res);
        p.cr = res;
        p.errors = res->errors;
        if (!res->ok) { p.state = -1; return; }
        // Metadata (reflection for the UI): the front end only. No module / layout /
        // pipeline is created for an effect that is not planned.
        if (p.metaOnly || !j.dev) { p.state = 2; return; }
        if (j.abandon && j.abandon->load()) { p.errors += "error: compile abandoned\n"; p.state = -1; return; }
        std::string err;
        j.stage = 1;
        const bool built = buildPipelines(j, err);
        j.stage = 0;
        if (!built)
        {
            p.errors += err + "\n";
            p.state = -1;
            return;
        }
        // Texture sources (decoded here, uploaded on the main thread at go-live).
        const reshadefx::effect_module& m = res->module;
        for (size_t i = 0; i < m.textures.size(); ++i)
        {
            if (j.abandon && j.abandon->load()) break;
            const std::string src = slfx::annotationString(m.textures[i].annotations, "source");
            if (src.empty() || p.tex[i].sem != SEM_NONE) continue;
            std::string werr;
            if (!loadSourceTexture(j.textureDirs, src, p.tex[i].w, p.tex[i].h, p.tex[i].pixels, werr))
                p.errors += werr + "\n";
        }
        p.state = 2;
    }
    void run()
    {
        // Named, so a hang can be attributed from /proc/<pid>/task/*/comm.
        LL_PROFILER_SET_THREAD_NAME("FX compile");
        ll_set_current_thread_name("FX compile");   // without Tracy the macro is a no-op (#736: Windows too)
        for (;;)
        {
            std::shared_ptr<Job> j;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this] { return quit || !queue.empty(); });
                if (quit) break;
                j = queue.front();
                queue.pop_front();
                busy = true;
                current = j;
                busySince = std::chrono::steady_clock::now();
            }
            try { execute(*j); }
            catch (...) { j->perm->errors += "error: internal error while compiling\n"; j->perm->state = -1; }
            {
                std::lock_guard<std::mutex> lk(m);
                // Abandoned or not, the perm comes back: the main thread destroys what the
                // job created (an orphan) -- even from a retired worker, if the device lives.
                done.push_back(j->perm);
                current.reset();
                busy = false;
            }
            idleCv.notify_all();
        }
        {
            std::lock_guard<std::mutex> lk(m);
            exited = true;
        }
        idleCv.notify_all();
    }
};

namespace
{
bool buildPipelines(LLFxRuntimeVK::Job& job, std::string& err)
{
    using Perm = LLFxRuntimeVK::Perm;
    Perm& p = *job.perm;
    const slfx::CompileResult& cr = *p.cr;
    const reshadefx::effect_module& m = cr.module;
    VkDevice dev = job.dev;

    // Textures: dimensions / formats (validated against the device later, at go-live).
    p.tex.resize(m.textures.size());
    const uint32_t maxEdge = std::min(kMaxTexEdge, job.limits.maxImageDimension2D);
    for (size_t i = 0; i < m.textures.size(); ++i)
    {
        const reshadefx::texture& t = m.textures[i];
        Perm::TexVK& tv = p.tex[i];
        tv.sem = semanticOf(t.semantic);
        if (tv.sem != SEM_NONE) continue;
        if (t.type != reshadefx::texture_type::texture_2d)
        { err = "error: texture '" + t.name + "': only 2D textures are supported"; return false; }
        tv.w = t.width; tv.h = t.height;
        if (tv.w == 0 || tv.h == 0 || tv.w > maxEdge || tv.h > maxEdge)
        { err = "error: texture '" + t.name + "' is " + std::to_string(t.width) + "x" + std::to_string(t.height)
                + " (limit " + std::to_string(maxEdge) + ")"; return false; }
        uint32_t maxLv = 1;
        while ((std::max(tv.w, tv.h) >> maxLv) > 0) ++maxLv;
        tv.levels = std::clamp<uint32_t>(t.levels, 1u, std::min(maxLv, kMaxMips));
        tv.fmt = fxFormat(t.format);
        if (t.format == reshadefx::texture_format::unknown) tv.fmt = VK_FORMAT_R8G8B8A8_UNORM;
        tv.srgbFmt = srgbOf(tv.fmt);
    }

    // Modules
    std::unordered_map<std::string, VkShaderModule> mods;
    for (const auto& kv : cr.spirv)
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = kv.second.size() * 4;
        ci.pCode = kv.second.data();
        VkShaderModule sm = VK_NULL_HANDLE;
        const VkResult r = vkCreateShaderModule(dev, &ci, nullptr, &sm);
        if (r != VK_SUCCESS) { err = vkErr("vkCreateShaderModule", r); return false; }
        p.modules.push_back(sm);
        mods[kv.first] = sm;
    }

    // Set 0: the uniform block (dynamic offset per frame-ring slot).
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_ALL;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &b;
        const VkResult r = vkCreateDescriptorSetLayout(dev, &li, nullptr, &p.dsl0);
        if (r != VK_SUCCESS) { err = vkErr("vkCreateDescriptorSetLayout", r); return false; }
    }

    auto texIndex = [&](const std::string& unique) -> int {
        for (size_t i = 0; i < m.textures.size(); ++i)
            if (m.textures[i].unique_name == unique) return (int)i;
        return -1;
    };

    p.techs.resize(m.techniques.size());
    for (size_t ti = 0; ti < m.techniques.size(); ++ti)
    {
        const reshadefx::technique& t = m.techniques[ti];
        Perm::TechVK& tv = p.techs[ti];
        tv.enabledInScreenshot = slfx::annotationBool(t.annotations, "enabled_in_screenshot", true);
        tv.point = slfx::annotationString(t.annotations, "slipstream_point") == "hdr" ? LLFxRuntimeVK::POINT_HDR
                                                                                       : p.point;
        if (t.passes.size() > kMaxPassesPerTech)
        { err = "error: technique '" + t.name + "' has more than " + std::to_string(kMaxPassesPerTech) + " passes"; return false; }
        tv.passes.resize(t.passes.size());
        for (size_t pi = 0; pi < t.passes.size(); ++pi)
        {
            const reshadefx::pass& ps = t.passes[pi];
            Perm::PassVK& pv = tv.passes[pi];
            pv.compute = !ps.cs_entry_point.empty();

            // Set 1 (combined image samplers) / set 2 (storage images) from the codegen's bindings.
            std::vector<VkDescriptorSetLayoutBinding> b1, b2;
            for (const auto& sb : ps.sampler_bindings)
            {
                VkDescriptorSetLayoutBinding b{};
                b.binding = sb.entry_point_binding; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_ALL;
                b1.push_back(b);
            }
            for (const auto& ub : ps.storage_bindings)
            {
                VkDescriptorSetLayoutBinding b{};
                b.binding = ub.entry_point_binding; b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_ALL;
                b2.push_back(b);
            }
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)b1.size(); li.pBindings = b1.data();
            VkResult r = vkCreateDescriptorSetLayout(dev, &li, nullptr, &pv.dsl1);
            if (r != VK_SUCCESS) { err = vkErr("vkCreateDescriptorSetLayout", r); return false; }
            li.bindingCount = (uint32_t)b2.size(); li.pBindings = b2.data();
            r = vkCreateDescriptorSetLayout(dev, &li, nullptr, &pv.dsl2);
            if (r != VK_SUCCESS) { err = vkErr("vkCreateDescriptorSetLayout", r); return false; }
            const VkDescriptorSetLayout sets[3] = {p.dsl0, pv.dsl1, pv.dsl2};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 3; pli.pSetLayouts = sets;
            r = vkCreatePipelineLayout(dev, &pli, nullptr, &pv.layout);
            if (r != VK_SUCCESS) { err = vkErr("vkCreatePipelineLayout", r); return false; }

            // Robust access on user shaders where the device allows it (#546).
            VkPipelineRobustnessCreateInfoEXT rob{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
            rob.storageBuffers = job.robustBuffer;
            rob.uniformBuffers = job.robustBuffer;
            rob.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
            rob.images = job.robustImage;

            if (pv.compute)
            {
                auto it = mods.find(ps.cs_entry_point);
                if (it == mods.end()) { err = "error: missing compute entry point " + ps.cs_entry_point; return false; }
                const VkPhysicalDeviceLimits& L = job.limits;
                pv.vpW = ps.viewport_width; pv.vpH = ps.viewport_height; pv.vpZ = std::max(1u, ps.viewport_dispatch_z);
                if (pv.vpW > L.maxComputeWorkGroupCount[0] || pv.vpH > L.maxComputeWorkGroupCount[1]
                    || pv.vpZ > L.maxComputeWorkGroupCount[2])
                { err = "error: pass '" + ps.name + "' dispatch size exceeds the device limit"; return false; }
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = it->second;
                ci.stage.pName = ps.cs_entry_point.c_str();
                ci.layout = pv.layout;
                if (job.robust) ci.pNext = &rob;
                if (job.abandon && job.abandon->load()) { err = "error: compile abandoned"; return false; }
                {
                    const VkPipelineCompileCrumb crumb(("FX compute pass " + ps.name).c_str());
                    r = vkCreateComputePipelines(dev, job.cache, 1, &ci, nullptr, &pv.pipe);
                }
                if (r != VK_SUCCESS) { pv.pipe = VK_NULL_HANDLE; err = vkErr("vkCreateComputePipelines", r) + " for pass '" + ps.name + "'"; return false; }
                continue;
            }

            // Graphics: render targets
            VkFormat colorFmts[8] = {};
            pv.srgbWrite = ps.srgb_write_enable;
            if (ps.render_target_names[0].empty())
            {
                pv.toBackBuffer = true;
                pv.rtCount = 1;
                pv.rt[0] = -1;
                pv.vpW = p.w; pv.vpH = p.h;
                const VkFormat s = srgbOf(p.bbFormat);
                colorFmts[0] = (pv.srgbWrite && s != VK_FORMAT_UNDEFINED) ? s : p.bbFormat;
            }
            else
            {
                for (uint32_t k = 0; k < 8 && !ps.render_target_names[k].empty(); ++k)
                {
                    const int ti2 = texIndex(ps.render_target_names[k]);
                    if (ti2 < 0 || p.tex[ti2].sem != SEM_NONE)
                    { err = "error: pass '" + ps.name + "' renders to an unknown or semantic texture"; return false; }
                    pv.rt[k] = ti2;
                    pv.rtCount = k + 1;
                    const Perm::TexVK& tt = p.tex[ti2];
                    colorFmts[k] = (pv.srgbWrite && tt.srgbFmt != VK_FORMAT_UNDEFINED) ? tt.srgbFmt : tt.fmt;
                }
                pv.vpW = ps.viewport_width; pv.vpH = ps.viewport_height;
            }
            // Stencil (ReShade: only on passes the size of the back buffer).
            pv.stencil = ps.stencil_enable && pv.vpW == p.w && pv.vpH == p.h;
            if (pv.stencil) tv.usesStencil = true;

            auto vit = mods.find(ps.vs_entry_point);
            auto fit = ps.ps_entry_point.empty() ? mods.end() : mods.find(ps.ps_entry_point);
            if (vit == mods.end()) { err = "error: missing vertex entry point " + ps.vs_entry_point; return false; }
            VkPipelineShaderStageCreateInfo st[2]{};
            uint32_t nst = 0;
            st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st[nst].stage = VK_SHADER_STAGE_VERTEX_BIT; st[nst].module = vit->second; st[nst].pName = ps.vs_entry_point.c_str();
            ++nst;
            if (fit != mods.end())
            {
                st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                st[nst].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[nst].module = fit->second; st[nst].pName = ps.ps_entry_point.c_str();
                ++nst;
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = topology(ps.topology);
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1; vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            if (pv.stencil)
            {
                ds.stencilTestEnable = VK_TRUE;
                VkStencilOpState so{};
                so.failOp = stencilOp(ps.stencil_fail_op);
                so.passOp = stencilOp(ps.stencil_pass_op);
                so.depthFailOp = stencilOp(ps.stencil_depth_fail_op);
                so.compareOp = compareOp(ps.stencil_comparison_func);
                so.compareMask = ps.stencil_read_mask;
                so.writeMask = ps.stencil_write_mask;
                so.reference = ps.stencil_reference_value;
                ds.front = so; ds.back = so;
            }
            VkPipelineColorBlendAttachmentState cb[8]{};
            for (uint32_t k = 0; k < pv.rtCount; ++k)
            {
                cb[k].blendEnable = ps.blend_enable[k] && !isIntFormat(colorFmts[k]);
                cb[k].srcColorBlendFactor = blendFactor(ps.source_color_blend_factor[k]);
                cb[k].dstColorBlendFactor = blendFactor(ps.dest_color_blend_factor[k]);
                cb[k].colorBlendOp = blendOp(ps.color_blend_op[k]);
                cb[k].srcAlphaBlendFactor = blendFactor(ps.source_alpha_blend_factor[k]);
                cb[k].dstAlphaBlendFactor = blendFactor(ps.dest_alpha_blend_factor[k]);
                cb[k].alphaBlendOp = blendOp(ps.alpha_blend_op[k]);
                cb[k].colorWriteMask = ps.render_target_write_mask[k] & 0xF;
            }
            VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cbs.attachmentCount = pv.rtCount; cbs.pAttachments = cb;
            const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rinfo.colorAttachmentCount = pv.rtCount; rinfo.pColorAttachmentFormats = colorFmts;
            // The stencil format is the runtime's choice (Shared::stencilFmt picks the same one).
            rinfo.stencilAttachmentFormat = pv.stencil ? job.stencilFormat : VK_FORMAT_UNDEFINED;
            if (job.robust) rinfo.pNext = &rob;
            VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gi.pNext = &rinfo;
            gi.stageCount = nst; gi.pStages = st;
            gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
            gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
            gi.pColorBlendState = &cbs; gi.pDynamicState = &dy; gi.layout = pv.layout;
            if (pv.stencil && rinfo.stencilAttachmentFormat == VK_FORMAT_UNDEFINED)
            { err = "error: pass '" + ps.name + "' needs a stencil buffer, which this device cannot provide"; return false; }
            if (job.abandon && job.abandon->load()) { err = "error: compile abandoned"; return false; }
            {
                const VkPipelineCompileCrumb crumb(("FX pass " + ps.name).c_str());
                r = vkCreateGraphicsPipelines(dev, job.cache, 1, &gi, nullptr, &pv.pipe);
            }
            if (r != VK_SUCCESS) { pv.pipe = VK_NULL_HANDLE; err = vkErr("vkCreateGraphicsPipelines", r) + " for pass '" + ps.name + "'"; return false; }
        }
    }
    return true;
}
}

// ============================================================================
// Singleton / lifecycle
// ============================================================================
LLFxRuntimeVK& LLFxRuntimeVK::instance()
{
    static LLFxRuntimeVK s;
    return s;
}

LLFxRuntimeVK::LLFxRuntimeVK() : mWorker(new Worker) {}

LLFxRuntimeVK::~LLFxRuntimeVK()
{
    // Static destruction at process exit. Never join a thread that is inside a driver
    // compile (that was an unbounded wait): stop every worker, give them kShutdownWaitMs
    // together, join the ones that exited and leak the rest -- detached, with their Worker
    // kept alive because the thread still uses it. The process exit ends them.
    if (mWorker) mRetired.push_back(std::move(mWorker));
    for (auto& w : mRetired) w->stop();
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kShutdownWaitMs);
    for (auto& w : mRetired)
    {
        bool exited;
        {
            std::unique_lock<std::mutex> lk(w->m);
            w->idleCv.wait_until(lk, until, [&] { return w->exited; });
            exited = w->exited;
        }
        if (exited) continue;   // joined by ~Worker below
        if (w->thread.joinable()) w->thread.detach();
        (void)w.release();      // leaked on purpose: the detached thread still uses it
    }
    mRetired.clear();
}

bool LLFxRuntimeVK::compileInDriver() const
{
    if (mStuckAtDetach) return true;
    auto inDriver = [](Worker& w) {
        std::lock_guard<std::mutex> lk(w.m);
        return w.busy && !w.exited && w.current && w.current->stage.load() == 1;
    };
    if (mWorker && inDriver(*mWorker)) return true;
    for (const auto& w : mRetired)
        if (w && inDriver(*w)) return true;
    return false;
}

std::vector<std::pair<std::string, std::string>> LLFxRuntimeVK::takeQuarantineRequests()
{
    std::vector<std::pair<std::string, std::string>> out;
    out.swap(mQuarantine);
    return out;
}

void LLFxRuntimeVK::setAvatarMaskProvider(std::function<VkImageView()> provider)
{
    std::lock_guard<std::mutex> lk(sMaskMutex);
    sMaskProvider = std::move(provider);
}

std::string LLFxRuntimeVK::lastRecordedSummary()
{
    std::lock_guard<std::mutex> lk(sLastMutex);
    return sLastRecorded;
}

void LLFxRuntimeVK::attachDevice(VkDeviceContext* ctx)
{
    if (mCtx == ctx) return;
    if (mCtx) detachDevice();
    mCtx = ctx;
    if (!ctx) return;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx->physicalDevice(), &props);
    mTimestampPeriod = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0.f;
    // VK_EXT_pipeline_robustness: user-shader pipelines request robust access (#546).
    mRobustness = ctx->pipelineRobustnessEnabled();
    // Private, in-memory cache for user-effect pipelines (see llfxruntimevk.h).
    mStuckAtDetach = false;
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (vkCreatePipelineCache(ctx->device(), &pci, nullptr, &mUserCache) != VK_SUCCESS) mUserCache = VK_NULL_HANDLE;
}

void LLFxRuntimeVK::detachDevice()
{
    if (!mCtx) return;
    // The compile thread may be creating pipelines on this device right now: drop the
    // queue, ask it to stop at its next check point and wait -- boundedly. A driver
    // compile cannot be interrupted; if one is still running after kShutdownWaitMs the
    // worker is retired (never joined) and compileInDriver() tells the renderer not to
    // destroy the device under it.
    {
        std::lock_guard<std::mutex> lk(mWorker->m);
        mWorker->queue.clear();
    }
    mWorker->abandon = true;
    const auto t0 = std::chrono::steady_clock::now();
    bool idle = mWorker->waitIdle(kShutdownWaitMs);
    for (auto& w : mRetired)
    {
        const int spent = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        idle = w->waitIdle(std::max(0, kShutdownWaitMs - spent)) && idle;
    }
    pollJobs();   // finished jobs: orphans are destroyed, the rest freed by freeAllGpu
    if (!idle)
    {
        mStuckAtDetach = true;
        LL_WARNS("FX") << "An effect compile is still running inside the graphics driver after "
                       << kShutdownWaitMs << " ms; leaving it (its objects and the device are reclaimed at exit)"
                       << LL_ENDL;
        retireWorker();
    }
    else
        mWorker->abandon = false;   // idle: reusable if a device is attached again
    freeAllGpu();      // destroys every perm except one still being built (state 1)
    mPerms.clear();    // that one belongs to the (stuck) worker: its objects die with the process
    destroyHost();
    if (mUserCache && !mStuckAtDetach) vkDestroyPipelineCache(mCtx->device(), mUserCache, nullptr);
    mUserCache = VK_NULL_HANDLE;
    mCtx = nullptr;
}

void LLFxRuntimeVK::retireWorker()
{
    std::deque<std::shared_ptr<Job>> rest;
    {
        std::lock_guard<std::mutex> lk(mWorker->m);
        rest.swap(mWorker->queue);
    }
    mWorker->stop();   // quit after the job in flight; abandon its results
    mRetired.push_back(std::move(mWorker));
    mWorker = std::make_unique<Worker>();
    for (auto& j : rest) mWorker->push(j);
}

void LLFxRuntimeVK::checkWatchdog()
{
    std::shared_ptr<Job> cur;
    double secs = 0.0;
    {
        std::lock_guard<std::mutex> lk(mWorker->m);
        if (!mWorker->busy || !mWorker->current) return;
        cur = mWorker->current;
        secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - mWorker->busySince).count();
    }
    if (secs < kCompileTimeoutSec) return;
    const std::string eff = cur->perm->effect;
    const bool inDriver = cur->stage.load() == 1;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "compile took too long (over %.0f s %s); stopped and quarantined",
                  kCompileTimeoutSec, inDriver ? "inside the graphics driver" : "in the effect compiler");
    const std::string reason = buf;
    LL_WARNS("FX") << fileName(eff) << ": " << reason << " (" << cur->perm->w << "x" << cur->perm->h
                   << (cur->perm->metaOnly ? ", metadata" : ", pipelines") << ")" << LL_ENDL;
    // The stuck worker keeps its thread (it cannot be interrupted) and is retired; a new
    // worker takes the queue, minus this effect's other jobs (they would hang the same way).
    retireWorker();
    {
        std::lock_guard<std::mutex> lk(mWorker->m);
        auto& q = mWorker->queue;
        q.erase(std::remove_if(q.begin(), q.end(),
                               [&](const std::shared_ptr<Job>& j) { return j->perm->effect == eff; }),
                q.end());
    }
    bool waited = false;
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        // This effect's perms: queued / in-flight ones are dropped (an in-flight one comes
        // back to pollJobs as an orphan if the driver ever returns); built ones are freed.
        if (it->second->effect != eff) { ++it; continue; }
        if (it->second->state != 0 && it->second->state != 1 && mCtx)
        {
            if (!waited) { vkDeviceWaitIdle(mCtx->device()); waited = true; }
            destroyPerm(*it->second);
        }
        it = mPerms.erase(it);
    }
    mTimedOut[eff] = reason;
    mQuarantine.emplace_back(eff, reason);
    setStatusError(eff, "error: " + reason);
}

void LLFxRuntimeVK::tick()
{
    pollJobs();   // metadata compiles complete even with effects off (the Effects page)
}

void LLFxRuntimeVK::setEnabled(bool on)
{
    if (mEnabled == on) return;
    mEnabled = on;
    if (!on && mCtx) freeAllGpu();
    bumpMeta();
}

void LLFxRuntimeVK::setPlan(const std::vector<PlanTech>& order, const std::map<std::string, EffectConfig>& configs,
                            const std::vector<std::string>& includeDirs, const std::vector<std::string>& textureDirs)
{
    // A changed effect configuration (defines / point) or search path invalidates its perms.
    bool dirsChanged = includeDirs != mIncludeDirs || textureDirs != mTextureDirs;
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        const std::string& eff = it->second->effect;
        auto old = mConfigs.find(eff);
        auto now = configs.find(eff);
        const bool changed = dirsChanged || (old == mConfigs.end()) != (now == configs.end())
            || (old != mConfigs.end() && now != configs.end()
                && (old->second.defines != now->second.defines || old->second.point != now->second.point));
        if (changed && it->second->state != 1)
        {
            if (mCtx) { vkDeviceWaitIdle(mCtx->device()); destroyPerm(*it->second); }
            it = mPerms.erase(it);
        }
        else
            ++it;
    }
    mPlan = order;
    mConfigs = configs;
    mIncludeDirs = includeDirs;
    mTextureDirs = textureDirs;
    ++mPlanGen;
    mWantNormals = false;
    mWantMask = false;
}

void LLFxRuntimeVK::setUniformValues(const std::string& effect, const LLSD& values)
{
    mValues[effect] = std::make_shared<LLSD>(values);
}

void LLFxRuntimeVK::reloadAll()
{
    mTimedOut.clear();   // the user asked for a retry
    if (mCtx) vkDeviceWaitIdle(mCtx->device());
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        if (it->second->state == 1) { ++it; continue; }   // the worker still owns it
        if (mCtx) destroyPerm(*it->second);
        it = mPerms.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        for (auto& kv : mStatus) { kv.second.compiled = kv.second.failed = false; kv.second.errors.clear(); }
    }
    bumpMeta();
}

void LLFxRuntimeVK::reloadEffect(const std::string& effect)
{
    mTimedOut.erase(effect);   // re-enabled after a quarantine: the user asked for a retry
    if (mCtx) vkDeviceWaitIdle(mCtx->device());
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        if (it->second->effect != effect || it->second->state == 1) { ++it; continue; }
        if (mCtx) destroyPerm(*it->second);
        it = mPerms.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        auto it = mStatus.find(effect);
        if (it != mStatus.end()) { it->second.compiled = it->second.failed = false; it->second.errors.clear(); it->second.goodFrames = 0; }
    }
    bumpMeta();
}

void LLFxRuntimeVK::requestMetadata(const std::string& effect)
{
    const uint32_t w = mMainW ? mMainW : 1920, h = mMainH ? mMainH : 1080;
    const VkFormat f = mMainFormat != VK_FORMAT_UNDEFINED ? mMainFormat : VK_FORMAT_B8G8R8A8_UNORM;
    auto cfg = mConfigs.find(effect);
    const uint8_t point = cfg != mConfigs.end() ? cfg->second.point : (uint8_t)POINT_DISPLAY;
    const std::string key = permKey(effect, w, h, point, mFarPlane);
    if (mPerms.count(key) || mTimedOut.count(effect)) return;
    // Compile only (no device objects): the UI's reflection. Replaced by a full compile
    // when the effect is enabled (it is keyed the same; a metaOnly perm is rebuilt).
    auto p = std::make_shared<Perm>();
    p->key = key; p->effect = effect; p->w = w; p->h = h; p->point = point; p->farPlane = mFarPlane;
    p->cfgKey = configKey(effect);
    p->bbFormat = f; p->metaOnly = true;
    auto job = std::make_shared<Job>();
    job->perm = p;
    job->req.path = effect;
    job->req.includeDirs = mIncludeDirs;
    if (cfg != mConfigs.end()) job->req.defines = cfg->second.defines;
    job->req.width = w; job->req.height = h;
    job->req.depthFarPlane = mFarPlane;
    mPerms[key] = p;
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        mStatus[effect].compiling = true;
    }
    mWorker->push(job);
}

void LLFxRuntimeVK::warmSize(uint32_t w, uint32_t h)
{
    if (!mEnabled || !mCtx || mSuppress > 0 || w == 0 || h == 0 || (w == mMainW && h == mMainH)) return;
    const VkFormat f = mMainFormat != VK_FORMAT_UNDEFINED ? mMainFormat : VK_FORMAT_B8G8R8A8_UNORM;
    // One warmed size at a time (dragging the snapshot size must not pile up compiles).
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        Perm& p = *it->second;
        const bool other = (p.w != mMainW || p.h != mMainH) && (p.w != w || p.h != h);
        if (other && p.state != 1 && p.state != 3) { destroyPerm(p); it = mPerms.erase(it); }
        else ++it;
    }
    for (const PlanTech& t : mPlan)
    {
        auto cfg = mConfigs.find(t.effect);
        findOrQueue(t.effect, w, h, cfg != mConfigs.end() ? cfg->second.point : (uint8_t)POINT_DISPLAY, f, false);
    }
}

LLFxRuntimeVK::EffectStatus LLFxRuntimeVK::status(const std::string& effect) const
{
    std::lock_guard<std::mutex> lk(mStatusMutex);
    auto it = mStatus.find(effect);
    return it == mStatus.end() ? EffectStatus{} : it->second;
}

float LLFxRuntimeVK::techniqueGpuMs(const std::string& effect, const std::string& technique) const
{
    auto it = mGpuMs.find(effect + "\n" + technique);
    return it == mGpuMs.end() ? 0.f : it->second;
}

std::vector<std::string> LLFxRuntimeVK::activeEffects() const
{
    return mActive;
}

void LLFxRuntimeVK::setStatusError(const std::string& effect, const std::string& err)
{
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        EffectStatus& s = mStatus[effect];
        s.failed = true;
        s.compiling = false;
        if (s.errors.find(err) == std::string::npos) s.errors += err + "\n";
    }
    LL_WARNS("FX") << fileName(effect) << ": " << err << LL_ENDL;
    bumpMeta();
}

// ============================================================================
// Permutations
// ============================================================================
std::string LLFxRuntimeVK::permKey(const std::string& effect, uint32_t w, uint32_t h, uint8_t point, float farPlane) const
{
    return effect + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(point) + "|"
         + std::to_string((int)farPlane);
}

std::string LLFxRuntimeVK::configKey(const std::string& effect) const
{
    std::string k;
    auto cfg = mConfigs.find(effect);
    if (cfg != mConfigs.end())
    {
        k += std::to_string(cfg->second.point) + "|";
        for (const auto& d : cfg->second.defines) k += d.first + "=" + d.second + ";";
    }
    for (const std::string& d : mIncludeDirs) k += "|" + d;
    for (const std::string& d : mTextureDirs) k += "|" + d;
    return k;
}

std::shared_ptr<LLFxRuntimeVK::Perm> LLFxRuntimeVK::findOrQueue(const std::string& effect, uint32_t w, uint32_t h,
                                                                uint8_t point, VkFormat bbFormat, bool sync)
{
    const std::string key = permKey(effect, w, h, point, mFarPlane);
    auto it = mPerms.find(key);
    if (it != mPerms.end())
    {
        Perm& p = *it->second;
        p.lastUsed = mFrameCounter;
        if (!(p.metaOnly && p.state != 1)) return it->second;
        mPerms.erase(it);   // reflection-only: rebuild with pipelines
    }
    if (mTimedOut.count(effect)) return nullptr;   // the watchdog stopped it (quarantined)
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        auto st = mStatus.find(effect);
        if (st != mStatus.end() && st->second.failed && w == mMainW && h == mMainH) return nullptr;
    }
    auto p = std::make_shared<Perm>();
    p->key = key; p->effect = effect; p->w = w; p->h = h; p->point = point; p->farPlane = mFarPlane;
    p->cfgKey = configKey(effect);
    p->bbFormat = point == POINT_HDR ? VK_FORMAT_R16G16B16A16_SFLOAT : bbFormat;
    p->lastUsed = mFrameCounter;
    auto job = std::make_shared<Job>();
    job->perm = p;
    job->req.path = effect;
    job->req.includeDirs = mIncludeDirs;
    auto cfg = mConfigs.find(effect);
    if (cfg != mConfigs.end()) job->req.defines = cfg->second.defines;
    job->req.width = w; job->req.height = h;
    job->req.depthFarPlane = mFarPlane;
    if (point == POINT_HDR)
    {
        job->req.colorSpace = 2;
        job->req.colorFormat = slfx::FX_FORMAT_R16G16B16A16_FLOAT;
        job->req.colorBitDepth = 16;
    }
    else
    {
        job->req.colorFormat = bbFormat == VK_FORMAT_R8G8B8A8_UNORM ? slfx::FX_FORMAT_R8G8B8A8_UNORM
                                                                     : slfx::FX_FORMAT_B8G8R8A8_UNORM;
    }
    if (mCtx)
    {
        job->dev = mCtx->device();
        job->phys = mCtx->physicalDevice();
        job->limits = mCtx->limits();
        job->robust = mRobustness;
        job->robustBuffer = mCtx->robustBufferBehavior();
        job->robustImage = mCtx->robustImageBehavior();
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(job->phys, &props);
        job->req.vendorId = props.vendorID;
        job->req.deviceId = props.deviceID;
        job->req.rendererId = 0x20000 | (VK_API_VERSION_MAJOR(props.apiVersion) << 12)
                            | (VK_API_VERSION_MINOR(props.apiVersion) << 8);
        // Stencil format for stencil passes (carried in the request: the worker has no ctx).
        VkFormat sf = VK_FORMAT_UNDEFINED;
        for (VkFormat c : {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT})
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(job->phys, c, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { sf = c; break; }
        }
        job->stencilFormat = sf;
    }
    job->textureDirs = mTextureDirs;
    job->cache = mUserCache;
    mPerms[key] = p;
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        mStatus[effect].compiling = true;
    }
    if (sync)
    {
        // A snapshot needs it now: first in line on the worker, and the main thread waits
        // for it -- boundedly (it used to compile right here, unbounded, on the main
        // thread). Not ready in time: this shot goes without it; the watchdog handles a
        // compile that never ends.
        mWorker->push(job, true);
        const auto until = std::chrono::steady_clock::now()
                         + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kSyncWaitSec));
        Worker& w = *mWorker;
        std::unique_lock<std::mutex> lk(w.m);
        w.idleCv.wait_until(lk, until, [&] {
            return w.exited || std::find(w.done.begin(), w.done.end(), p) != w.done.end();
        });
    }
    else
        mWorker->push(job);
    return p;
}

void LLFxRuntimeVK::pollJobs()
{
    checkWatchdog();
    std::vector<std::shared_ptr<Perm>> done;
    {
        std::lock_guard<std::mutex> lk(mWorker->m);
        done.swap(mWorker->done);
    }
    // Retired workers: whatever they finally return is an orphan (destroyed below); drop
    // the ones whose thread has exited (~Worker joins a finished thread).
    for (auto it = mRetired.begin(); it != mRetired.end();)
    {
        Worker& w = **it;
        bool exited;
        {
            std::lock_guard<std::mutex> lk(w.m);
            for (auto& p : w.done) done.push_back(p);
            w.done.clear();
            exited = w.exited;
        }
        if (exited) it = mRetired.erase(it);
        else ++it;
    }
    if (done.empty()) return;
    for (auto& p : done)
    {
        // A perm dropped while compiling (reload / plan change): free its objects now.
        auto it = mPerms.find(p->key);
        bool orphan = it == mPerms.end() || it->second != p;
        // Its definitions / point / folders changed while it compiled: stale, recompile.
        if (!orphan && p->cfgKey != configKey(p->effect))
        {
            mPerms.erase(it);
            orphan = true;
        }
        {
            std::lock_guard<std::mutex> lk(mStatusMutex);
            EffectStatus& s = mStatus[p->effect];
            s.compiling = false;
            // Other sizes (snapshots, warmed sizes) do not overwrite the live view's status.
            const bool mainSize = p->metaOnly || mMainW == 0 || (p->w == mMainW && p->h == mMainH);
            if (!orphan && mainSize)
            {
                s.errors = p->errors;
                s.compiled = p->state == 2;
                s.failed = p->state == -1;
                if (p->cr) s.meta = p->cr;
            }
        }
        // Say which kind: a metadata (reflection) job never touches the driver, a pipeline
        // job does -- a shutdown / hang report has to tell them apart.
        const char* kind = p->metaOnly ? "metadata" : "pipelines";
        if (p->state == -1)
            LL_WARNS("FX") << fileName(p->effect) << " failed to compile (" << kind << (orphan ? ", stale request" : "")
                           << "):\n" << p->errors << LL_ENDL;
        else
            LL_INFOS("FX") << fileName(p->effect) << " compiled (" << kind << ") for " << p->w << "x" << p->h
                           << " in " << (p->cr ? p->cr->ms : 0.0) << " ms" << LL_ENDL;
        if (orphan && mCtx) destroyPerm(*p);
    }
    bumpMeta();
}

void LLFxRuntimeVK::destroyPerm(Perm& p)
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    for (Perm::TechVK& t : p.techs)
        for (Perm::PassVK& pv : t.passes)
        {
            if (pv.pipe) vkDestroyPipeline(dev, pv.pipe, nullptr);
            if (pv.layout) vkDestroyPipelineLayout(dev, pv.layout, nullptr);
            if (pv.dsl1) vkDestroyDescriptorSetLayout(dev, pv.dsl1, nullptr);
            if (pv.dsl2) vkDestroyDescriptorSetLayout(dev, pv.dsl2, nullptr);
            pv = Perm::PassVK{};
        }
    p.techs.clear();
    for (Perm::TexVK& t : p.tex)
    {
        for (VkImageView& v : t.mipViews) if (v) mCtx->destroyView(v);
        if (t.srgbView) mCtx->destroyView(t.srgbView);
        if (t.rtSrgb) mCtx->destroyView(t.rtSrgb);
        if (t.img.image) mCtx->destroyImage(t.img);
        t = Perm::TexVK{};
    }
    p.tex.clear();
    for (VkShaderModule m : p.modules) vkDestroyShaderModule(dev, m, nullptr);
    p.modules.clear();
    if (p.pool) vkDestroyDescriptorPool(dev, p.pool, nullptr);
    p.pool = VK_NULL_HANDLE; p.set0 = VK_NULL_HANDLE;
    if (p.dsl0) vkDestroyDescriptorSetLayout(dev, p.dsl0, nullptr);
    p.dsl0 = VK_NULL_HANDLE;
    if (p.ubo.buffer) mCtx->destroyBuffer(p.ubo);
    p.ubo = GpuBuffer{};
    p.vramBytes = 0;
    if (p.state == 3) p.state = 2;
}

void LLFxRuntimeVK::destroyShared(Shared& s)
{
    if (!mCtx) return;
    VkDevice dev = mCtx->device();
    if (s.bbRTSrgb) mCtx->destroyView(s.bbRTSrgb);
    if (s.bbTexSrgb) mCtx->destroyView(s.bbTexSrgb);
    for (GpuImage* g : {&s.bbRT, &s.bbTex, &s.scopeSave, &s.stencil})
        if (g->image) mCtx->destroyImage(*g);
    for (GpuImage& g : s.input) if (g.image) mCtx->destroyImage(g);
    if (s.pool) vkDestroyDescriptorPool(dev, s.pool, nullptr);
    s = Shared{};
}

void LLFxRuntimeVK::freeAllGpu()
{
    if (!mCtx) return;
    vkDeviceWaitIdle(mCtx->device());
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        if (it->second->state == 1) { ++it; continue; }   // pollJobs frees it as an orphan
        destroyPerm(*it->second);
        it = mPerms.erase(it);
    }
    for (auto& s : mShared) destroyShared(*s);
    mShared.clear();
    for (int i = 0; i < 2; ++i)
    {
        if (mQueryPool[i]) vkDestroyQueryPool(mCtx->device(), mQueryPool[i], nullptr);
        mQueryPool[i] = VK_NULL_HANDLE;
        mTimerIssued[i] = false;
        mTimedTechs[i].clear();
    }
    std::lock_guard<std::mutex> lk(mStatusMutex);
    for (auto& kv : mStatus) { kv.second.vramBytes = 0; kv.second.goodFrames = 0; }
}

// ============================================================================
// Host objects
// ============================================================================
bool LLFxRuntimeVK::initHost()
{
    if (mHost) return true;
    if (mHostFailed || !mCtx) return false;
    VkDevice dev = mCtx->device();
    auto h = std::make_unique<Host>();
    bool ok = true;
    auto chk = [&](VkResult r, const char* what) {
        if (r != VK_SUCCESS) { ok = false; LL_WARNS("FX") << what << " failed: " << vkResultString(r) << LL_ENDL; }
    };
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.f;
    chk(vkCreateSampler(dev, &si, nullptr, &h->nearest), "FX sampler");
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    chk(vkCreateSampler(dev, &si, nullptr, &h->linear), "FX sampler");

    auto makeDsl = [&](std::initializer_list<VkDescriptorType> types, VkDescriptorSetLayout* out) {
        std::vector<VkDescriptorSetLayoutBinding> b;
        uint32_t i = 0;
        for (VkDescriptorType t : types)
        {
            VkDescriptorSetLayoutBinding x{};
            x.binding = i++; x.descriptorType = t; x.descriptorCount = 1; x.stageFlags = VK_SHADER_STAGE_ALL;
            b.push_back(x);
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)b.size(); li.pBindings = b.data();
        chk(vkCreateDescriptorSetLayout(dev, &li, nullptr, out), "FX set layout");
    };
    const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, STO = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    makeDsl({CIS}, &h->blitDsl);
    makeDsl({CIS, CIS, CIS}, &h->scopeDsl);
    makeDsl({CIS, CIS, STO, STO}, &h->resolveDsl);
    makeDsl({CIS, STO}, &h->hdrDsl);
    auto makeLayout = [&](VkDescriptorSetLayout dsl, uint32_t push, VkShaderStageFlags stages, VkPipelineLayout* out) {
        VkPushConstantRange pr{stages, 0, push};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &dsl;
        li.pushConstantRangeCount = push ? 1 : 0; li.pPushConstantRanges = &pr;
        chk(vkCreatePipelineLayout(dev, &li, nullptr, out), "FX pipeline layout");
    };
    makeLayout(h->blitDsl, 0, 0, &h->blitLayout);
    makeLayout(h->scopeDsl, 16, VK_SHADER_STAGE_FRAGMENT_BIT, &h->scopeLayout);
    makeLayout(h->resolveDsl, 80, VK_SHADER_STAGE_COMPUTE_BIT, &h->resolveLayout);
    makeLayout(h->hdrDsl, 16, VK_SHADER_STAGE_FRAGMENT_BIT, &h->hdrLayout);

    auto makeModule = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes; ci.pCode = code;
        chk(vkCreateShaderModule(dev, &ci, nullptr, out), "FX shader module");
    };
    makeModule(kFxFullscreenVertSpv, kFxFullscreenVertSpv_bytes, &h->vs);
    makeModule(kFxBlitFragSpv, kFxBlitFragSpv_bytes, &h->blitFs);
    makeModule(kFxScopeFragSpv, kFxScopeFragSpv_bytes, &h->scopeFs);
    makeModule(kFxHdrInFragSpv, kFxHdrInFragSpv_bytes, &h->hdrInFs);
    makeModule(kFxHdrOutFragSpv, kFxHdrOutFragSpv_bytes, &h->hdrOutFs);
    const uint32_t* rcode[IN_COUNT] = {kFxResolveDepthSpv, kFxResolveNormalSpv, kFxResolveMotionSpv,
                                       kFxResolveMaskSpv, kFxResolveHdrSpv};
    const size_t rbytes[IN_COUNT] = {kFxResolveDepthSpv_bytes, kFxResolveNormalSpv_bytes, kFxResolveMotionSpv_bytes,
                                     kFxResolveMaskSpv_bytes, kFxResolveHdrSpv_bytes};
    for (int i = 0; i < IN_COUNT && ok; ++i)
    {
        VkShaderModule cs = VK_NULL_HANDLE;
        makeModule(rcode[i], rbytes[i], &cs);
        if (!cs) break;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = cs; ci.stage.pName = "main";
        ci.layout = h->resolveLayout;
        {
            const VkPipelineCompileCrumb crumb("FX resolve pipeline");   // (#782 crash breadcrumb)
            chk(vkCreateComputePipelines(dev, vkSharedPipelineCache(), 1, &ci, nullptr, &h->resolve[i]), "FX resolve pipeline");
        }
        vkDestroyShaderModule(dev, cs, nullptr);
    }
    // 1x1 placeholders, GENERAL.
    if (ok)
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        h->dummyColor = mCtx->createImage2D(1, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        h->dummyR32F = mCtx->createImage2D(1, 1, VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        ok = h->dummyColor.image && h->dummyR32F.image;
        if (ok)
            mCtx->submitImmediate([&](VkCommandBuffer c) {
                for (VkImage im : {h->dummyColor.image, h->dummyR32F.image})
                {
                    imageBarrier(c, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    VkClearColorValue z{};
                    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(c, im, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
                }
                barrierAll(c);
            });
    }
    mHost = std::move(h);
    if (!ok)
    {
        LL_WARNS("FX") << "effects host init failed; effects are unavailable this session" << LL_ENDL;
        destroyHost();
        mHostFailed = true;
        return false;
    }
    return true;
}

void LLFxRuntimeVK::destroyHost()
{
    if (!mHost || !mCtx) { mHost.reset(); return; }
    VkDevice dev = mCtx->device();
    Host& h = *mHost;
    for (VkSampler s : {h.nearest, h.linear}) if (s) vkDestroySampler(dev, s, nullptr);
    for (auto& kv : h.samplerCache) vkDestroySampler(dev, kv.second, nullptr);
    for (auto* v : {&h.blitPipes, &h.scopePipes, &h.hdrInPipes, &h.hdrOutPipes})
        for (auto& kv : *v) vkDestroyPipeline(dev, kv.second, nullptr);
    for (VkPipeline p : h.resolve) if (p) vkDestroyPipeline(dev, p, nullptr);
    for (VkPipelineLayout l : {h.blitLayout, h.scopeLayout, h.resolveLayout, h.hdrLayout})
        if (l) vkDestroyPipelineLayout(dev, l, nullptr);
    for (VkDescriptorSetLayout l : {h.blitDsl, h.scopeDsl, h.resolveDsl, h.hdrDsl})
        if (l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    for (VkShaderModule m : {h.vs, h.blitFs, h.scopeFs, h.hdrInFs, h.hdrOutFs})
        if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (h.dummyColor.image) mCtx->destroyImage(h.dummyColor);
    if (h.dummyR32F.image) mCtx->destroyImage(h.dummyR32F);
    mHost.reset();
}

namespace
{
VkPipeline hostGraphicsPipe(VkDevice dev, VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkFormat fmt)
{
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1; cbs.pAttachments = &cb;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = 2; gi.pStages = st;
    gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cbs; gi.pDynamicState = &dy; gi.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    const VkPipelineCompileCrumb crumb("FX graphics pipeline");   // (#782 crash breadcrumb)
    if (vkCreateGraphicsPipelines(dev, vkSharedPipelineCache(), 1, &gi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}

VkPipeline cachedPipe(std::vector<std::pair<VkFormat, VkPipeline>>& cache, VkFormat fmt, const std::function<VkPipeline()>& make)
{
    for (auto& kv : cache) if (kv.first == fmt) return kv.second;
    VkPipeline p = make();
    if (p) cache.emplace_back(fmt, p);
    return p;
}

void drawFullscreen(VkCommandBuffer cmd, VkImageView dst, VkImageLayout layout, uint32_t w, uint32_t h,
                    VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set, const void* push, uint32_t pushSize)
{
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = dst; ca.imageLayout = layout;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, nullptr);
    if (pushSize) vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void writeImage(VkDevice dev, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkSampler s, VkImageView v)
{
    VkDescriptorImageInfo ii{s, v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set; w.dstBinding = binding; w.descriptorCount = 1; w.descriptorType = type; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
}
}

// ============================================================================
// Shared per-(size, point) targets
// ============================================================================
LLFxRuntimeVK::Shared* LLFxRuntimeVK::ensureShared(uint32_t w, uint32_t h, uint8_t point, VkFormat bbFormat)
{
    for (auto& s : mShared)
        if (s->w == w && s->h == h && s->point == point && s->bbFormat == bbFormat)
        {
            s->lastUsed = mFrameCounter;
            return s.get();
        }
    if (!initHost()) return nullptr;
    VkMemScope memcat(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mCtx);
    auto s = std::make_unique<Shared>();
    s->w = w; s->h = h; s->point = point; s->bbFormat = bbFormat; s->bbSrgb = srgbOf(bbFormat);
    const VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                              | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    s->bbRT = mCtx->createImage2D(w, h, bbFormat, u, VK_IMAGE_ASPECT_COLOR_BIT, 1, s->bbSrgb);
    s->bbTex = mCtx->createImage2D(w, h, bbFormat, u, VK_IMAGE_ASPECT_COLOR_BIT, 1, s->bbSrgb);
    if (!s->bbRT.image || !s->bbTex.image)
    {
        destroyShared(*s);
        return nullptr;
    }
    mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)s->bbRT.image, "fxBackBuffer");
    mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)s->bbTex.image, "fxBackBufferTex");
    if (s->bbSrgb != VK_FORMAT_UNDEFINED)
    {
        s->bbRTSrgb = mCtx->createView(s->bbRT.image, s->bbSrgb, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        s->bbTexSrgb = mCtx->createView(s->bbTex.image, s->bbSrgb, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    }
    s->vramBytes = mCtx->allocBytes(s->bbRT) + mCtx->allocBytes(s->bbTex);
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 48}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 32; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mCtx->device(), &pi, nullptr, &s->pool) != VK_SUCCESS)
    {
        destroyShared(*s);
        return nullptr;
    }
    VkDevice dev = mCtx->device();
    auto alloc = [&](VkDescriptorSetLayout l, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s->pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        return vkAllocateDescriptorSets(dev, &ai, out) == VK_SUCCESS;
    };
    bool ok = alloc(mHost->blitDsl, &s->blitSet);
    for (int k = 0; k < IN_COUNT; ++k)
        for (uint32_t r = 0; r < kRingMax; ++r) ok = ok && alloc(mHost->resolveDsl, &s->resolveSet[k][r]);
    for (uint32_t r = 0; r < kRingMax; ++r)
        ok = ok && alloc(mHost->hdrDsl, &s->hdrInSet[r]) && alloc(mHost->hdrDsl, &s->hdrOutSet[r]);
    ok = ok && alloc(mHost->scopeDsl, &s->scopeSet);
    if (!ok)
    {
        destroyShared(*s);
        return nullptr;
    }
    writeImage(dev, s->blitSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mHost->nearest, s->bbRT.view);
    mCtx->submitImmediate([&](VkCommandBuffer c) {
        imageBarrier(c, s->bbTex.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
        // The HDR point draws its back buffer itself: GENERAL for good.
        if (point == POINT_HDR)
            imageBarrier(c, s->bbRT.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
    });
    s->lastUsed = mFrameCounter;
    LL_INFOS("FX") << "effects targets " << w << "x" << h << (point == POINT_HDR ? " (HDR point)" : "") << LL_ENDL;
    mShared.push_back(std::move(s));
    return mShared.back().get();
}

bool LLFxRuntimeVK::ensureSharedInput(Shared& s, int kind)
{
    if (s.input[kind].image) return true;
    VkMemScope memcat(VKMEM_TARGET);
    VkDeviceContext::SoftAllocScope soft(mCtx);
    s.input[kind] = mCtx->createImage2D(s.w, s.h, kInputFormat[kind],
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!s.input[kind].image) return false;
    static const char* kNames[IN_COUNT] = {"fxDepth", "fxNormal", "fxMotion", "fxAvatarMask", "fxSceneHdr"};
    mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)s.input[kind].image, kNames[kind]);
    s.vramBytes += mCtx->allocBytes(s.input[kind]);
    GpuImage& g = s.input[kind];
    mCtx->submitImmediate([&](VkCommandBuffer c) {
        imageBarrier(c, g.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue z{};
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, g.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
        barrierAll(c);
    });
    return true;
}

// ============================================================================
// Go-live: images, samplers, descriptor sets, uniform buffer
// ============================================================================
bool LLFxRuntimeVK::ensureLive(Perm& p, Shared& s, const Frame& f, std::string& err)
{
    if (p.state == 3) return true;
    if (p.state != 2 || p.metaOnly) return false;
    VkDevice dev = mCtx->device();
    VkPhysicalDevice phys = mCtx->physicalDevice();
    const reshadefx::effect_module& m = p.cr->module;
    VkMemScope memcat(VKMEM_TARGET);

    // Pre-check every own texture (format support, size) and its memory, before creating any.
    uint64_t need = 0;
    for (size_t i = 0; i < p.tex.size(); ++i)
    {
        Perm::TexVK& t = p.tex[i];
        if (t.sem != SEM_NONE) continue;
        const reshadefx::texture& rt = m.textures[i];
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (rt.render_target) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (rt.storage_access) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        VkImageFormatProperties ifp{};
        const VkImageCreateFlags flags = t.srgbFmt != VK_FORMAT_UNDEFINED ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        const VkResult r = vkGetPhysicalDeviceImageFormatProperties(phys, t.fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                                    usage, flags, &ifp);
        if (r != VK_SUCCESS || ifp.maxExtent.width < t.w || ifp.maxExtent.height < t.h || ifp.maxMipLevels < t.levels)
        {
            err = "error: texture '" + rt.name + "': format / usage / size not supported by this GPU";
            return false;
        }
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys, t.fmt, &fp);
        t.linearBlit = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                    && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
                    && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        need += mCtx->imageMemoryBytes(t.w, t.h, 1, t.levels, t.fmt, usage, false);
    }
    const uint64_t budget = budgetBytes(f);
    if (budget && need > budget)
    {
        err = "error: needs " + std::to_string(need >> 20) + " MB of video memory for its textures; the effects budget allows "
            + std::to_string(budget >> 20) + " MB";
        return false;
    }

    // Images + views
    std::vector<VkBufferImageCopy> uploads;
    for (size_t i = 0; i < p.tex.size(); ++i)
    {
        Perm::TexVK& t = p.tex[i];
        if (t.sem != SEM_NONE) continue;
        const reshadefx::texture& rt = m.textures[i];
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (rt.render_target) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (rt.storage_access) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        {
            VkDeviceContext::SoftAllocScope soft(mCtx);
            t.img = mCtx->createImage2D(t.w, t.h, t.fmt, usage, VK_IMAGE_ASPECT_COLOR_BIT, t.levels, t.srgbFmt);
        }
        if (!t.img.image) { err = "error: out of video memory for texture '" + rt.name + "'"; return false; }
        p.vramBytes += mCtx->allocBytes(t.img);
        const std::string dbg = "fx:" + fileName(p.effect) + ":" + rt.name;
        mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)t.img.image, dbg.c_str());
        for (uint32_t l = 0; l < t.levels; ++l)
            t.mipViews.push_back(mCtx->createView(t.img.image, t.fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1));
        if (t.srgbFmt != VK_FORMAT_UNDEFINED)
        {
            t.srgbView = mCtx->createView(t.img.image, t.srgbFmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, 1);
            t.rtSrgb = mCtx->createView(t.img.image, t.srgbFmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        }
    }
    // Initial layout (GENERAL, cleared) + `source` uploads + their mips, in one submit.
    {
        uint64_t staged = 0;
        for (const Perm::TexVK& t : p.tex) if (!t.pixels.empty() && t.fmt == VK_FORMAT_R8G8B8A8_UNORM) staged += t.pixels.size();
        GpuBuffer stage;
        if (staged)
        {
            VkMemScope stg(VKMEM_STAGING);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            stage = mCtx->createBuffer(staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (!stage.buffer || !stage.mapped) { err = "error: out of memory staging the texture files"; return false; }
        }
        mCtx->submitImmediate([&](VkCommandBuffer c) {
            uint64_t off = 0;
            for (Perm::TexVK& t : p.tex)
            {
                if (t.sem != SEM_NONE || !t.img.image) continue;
                imageBarrier(c, t.img.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
                VkClearColorValue z{};
                const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, 1};
                vkCmdClearColorImage(c, t.img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
                barrierAll(c);
                if (!t.pixels.empty() && t.fmt == VK_FORMAT_R8G8B8A8_UNORM && stage.mapped)
                {
                    std::memcpy((uint8_t*)stage.mapped + off, t.pixels.data(), t.pixels.size());
                    VkBufferImageCopy bc{};
                    bc.bufferOffset = off;
                    bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    bc.imageExtent = {t.w, t.h, 1};
                    vkCmdCopyBufferToImage(c, stage.buffer, t.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
                    off += t.pixels.size();
                    barrierAll(c);
                    for (uint32_t l = 1; l < t.levels && t.linearBlit; ++l)
                    {
                        VkImageBlit b{};
                        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                        b.srcOffsets[1] = {(int32_t)std::max(1u, t.w >> (l - 1)), (int32_t)std::max(1u, t.h >> (l - 1)), 1};
                        b.dstOffsets[1] = {(int32_t)std::max(1u, t.w >> l), (int32_t)std::max(1u, t.h >> l), 1};
                        vkCmdBlitImage(c, t.img.image, VK_IMAGE_LAYOUT_GENERAL, t.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
                        barrierAll(c);
                    }
                }
                else if (!t.pixels.empty())
                {
                    // `source` on a non-RGBA8 texture: ReShade converts; we leave it cleared.
                }
            }
        });
        if (stage.buffer) mCtx->destroyBuffer(stage);
        for (Perm::TexVK& t : p.tex) { t.pixels.clear(); t.pixels.shrink_to_fit(); }
    }

    // Samplers (shared cache, by state)
    p.samplers.resize(m.samplers.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < m.samplers.size(); ++i)
    {
        const reshadefx::sampler& sd = m.samplers[i];
        SamplerKey k{(uint8_t)sd.filter, (uint8_t)sd.address_u, (uint8_t)sd.address_v, (uint8_t)sd.address_w,
                     sd.lod_bias, sd.min_lod, sd.max_lod};
        // Integer textures cannot be filtered: force point sampling.
        const int ti = [&] { for (size_t j = 0; j < m.textures.size(); ++j) if (m.textures[j].unique_name == sd.texture_name) return (int)j; return -1; }();
        if (ti >= 0 && p.tex[ti].sem == SEM_NONE && isIntFormat(p.tex[ti].fmt)) k.filter = (uint8_t)reshadefx::filter_mode::min_mag_mip_point;
        VkSampler found = VK_NULL_HANDLE;
        for (auto& kv : mHost->samplerCache) if (kv.first == k) { found = kv.second; break; }
        if (!found)
        {
            const uint8_t fm = k.filter;
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.minFilter = (fm & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.magFilter = (fm & 0x04) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = (fm & 0x01) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            if (fm == (uint8_t)reshadefx::filter_mode::anisotropic)
            {
                si.minFilter = si.magFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            }
            si.addressModeU = addressMode(sd.address_u);
            si.addressModeV = addressMode(sd.address_v);
            si.addressModeW = addressMode(sd.address_w);
            si.mipLodBias = std::clamp(sd.lod_bias, -15.f, 15.f);
            si.minLod = std::max(sd.min_lod, 0.f);
            si.maxLod = std::min(sd.max_lod, 1000.f);
            si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            const VkResult r = vkCreateSampler(dev, &si, nullptr, &found);
            if (r != VK_SUCCESS) { err = vkErr("vkCreateSampler", r); return false; }
            mHost->samplerCache.emplace_back(k, found);
        }
        p.samplers[i] = found;
    }

    // Semantic inputs this effect reads (shared, resolved per frame).
    for (const Perm::TexVK& t : p.tex)
    {
        int kind = -1;
        switch (t.sem)
        {
        case SEM_DEPTH: kind = IN_DEPTH; break;
        case SEM_NORMAL: kind = IN_NORMAL; break;
        case SEM_MOTION: kind = IN_MOTION; break;
        case SEM_MASK: kind = IN_MASK; break;
        case SEM_HDR: kind = IN_HDR; break;
        default: break;
        }
        if (kind >= 0 && !ensureSharedInput(s, kind)) { err = "error: out of video memory for an input texture"; return false; }
    }

    // Descriptor pool + sets
    uint32_t nCis = 0, nSto = 0, nSets = 1;
    for (size_t ti = 0; ti < m.techniques.size(); ++ti)
        for (const reshadefx::pass& ps : m.techniques[ti].passes)
        {
            nCis += (uint32_t)ps.sampler_bindings.size();
            nSto += (uint32_t)ps.storage_bindings.size();
            nSets += 2;
        }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, nCis)},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, std::max(1u, nSto)}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = nSets; pi.poolSizeCount = 3; pi.pPoolSizes = sizes;
    VkResult r = vkCreateDescriptorPool(dev, &pi, nullptr, &p.pool);
    if (r != VK_SUCCESS) { err = vkErr("vkCreateDescriptorPool", r); return false; }
    auto alloc = [&](VkDescriptorSetLayout l, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = p.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
        return vkAllocateDescriptorSets(dev, &ai, out);
    };
    if ((r = alloc(p.dsl0, &p.set0)) != VK_SUCCESS) { err = vkErr("vkAllocateDescriptorSets", r); return false; }

    // Uniform buffer: one region per frame-ring slot.
    const VkDeviceSize align = std::max<VkDeviceSize>(16, mCtx->limits().minUniformBufferOffsetAlignment);
    const uint32_t raw = std::max(16u, (m.total_uniform_size + 15u) & ~15u);
    p.uboStride = (uint32_t)((raw + align - 1) / align * align);
    {
        VkMemScope ubo(VKMEM_STAGING);
        VkDeviceContext::SoftAllocScope soft(mCtx);
        p.ubo = mCtx->createBuffer((VkDeviceSize)p.uboStride * kRingMax, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (!p.ubo.buffer || !p.ubo.mapped) { err = "error: out of memory for the uniform buffer"; return false; }
    p.uboData.assign(p.uboStride, 0);
    {
        VkDescriptorBufferInfo bi{p.ubo.buffer, 0, p.uboStride};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p.set0; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    auto texIndex = [&](const std::string& unique) -> int {
        for (size_t i = 0; i < m.textures.size(); ++i) if (m.textures[i].unique_name == unique) return (int)i;
        return -1;
    };
    // Sampled view of module texture `ti` (srgb: the SRGB view where one exists).
    auto sampledView = [&](int ti, bool srgb) -> VkImageView {
        if (ti < 0) return mHost->dummyColor.view;
        const Perm::TexVK& t = p.tex[ti];
        switch (t.sem)
        {
        case SEM_NONE: return (srgb && t.srgbView) ? t.srgbView : t.img.view;
        case SEM_COLOR: return (srgb && s.bbTexSrgb) ? s.bbTexSrgb : s.bbTex.view;
        case SEM_DEPTH: return s.input[IN_DEPTH].view;
        case SEM_NORMAL: return s.input[IN_NORMAL].view;
        case SEM_MOTION: return s.input[IN_MOTION].view;
        case SEM_MASK: return s.input[IN_MASK].view;
        case SEM_HDR: return s.input[IN_HDR].view;
        default: return mHost->dummyColor.view;
        }
    };
    for (size_t i = 0; i < p.tex.size(); ++i)
        if (p.tex[i].sem == SEM_UNKNOWN)
        {
            const std::string warn = "warning: texture semantic '" + m.textures[i].semantic + "' is not provided (bound to black)";
            if (p.errors.find(warn) == std::string::npos) p.errors += warn + "\n";
        }

    for (size_t ti = 0; ti < m.techniques.size(); ++ti)
        for (size_t pi2 = 0; pi2 < m.techniques[ti].passes.size(); ++pi2)
        {
            const reshadefx::pass& ps = m.techniques[ti].passes[pi2];
            Perm::PassVK& pv = p.techs[ti].passes[pi2];
            if ((r = alloc(pv.dsl1, &pv.set1)) != VK_SUCCESS || (r = alloc(pv.dsl2, &pv.set2)) != VK_SUCCESS)
            { err = vkErr("vkAllocateDescriptorSets", r); return false; }
            for (size_t k = 0; k < ps.sampler_bindings.size(); ++k)
            {
                const reshadefx::sampler_binding& sb = ps.sampler_bindings[k];
                const reshadefx::sampler& sd = m.samplers[sb.index];
                const int t = texIndex(sd.texture_name);
                const bool srgb = ps.texture_bindings.size() > k ? ps.texture_bindings[k].srgb : sd.srgb;
                const bool intTex = t >= 0 && p.tex[t].sem == SEM_NONE && isIntFormat(p.tex[t].fmt);
                writeImage(dev, pv.set1, sb.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           intTex ? mHost->nearest : p.samplers[sb.index], sampledView(t, srgb));
            }
            for (const reshadefx::storage_binding& ub : ps.storage_bindings)
            {
                const reshadefx::storage& sd = m.storages[ub.index];
                const int t = texIndex(sd.texture_name);
                if (t < 0 || p.tex[t].sem != SEM_NONE || sd.level >= p.tex[t].levels)
                { err = "error: storage '" + sd.name + "' names an unusable texture"; return false; }
                writeImage(dev, pv.set2, ub.entry_point_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE,
                           p.tex[t].mipViews[sd.level]);
            }
        }

    // Uniform special state
    p.pingpong.assign(m.uniforms.size() * 2, 0.f);
    for (size_t i = 0; i < m.uniforms.size(); ++i)
        p.pingpong[i * 2 + 1] = 1.f;
    p.keyToggle.assign(m.uniforms.size(), 0);
    p.keyPrev.assign(m.uniforms.size(), 0);
    p.goodFrames = 0;
    p.state = 3;
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        EffectStatus& st = mStatus[p.effect];
        st.vramBytes = 0;
        for (auto& kv : mPerms) if (kv.second->effect == p.effect) st.vramBytes += kv.second->vramBytes;
        st.errors = p.errors;
    }
    LL_INFOS("FX") << fileName(p.effect) << " live at " << p.w << "x" << p.h << ", " << (p.vramBytes >> 20) << " MB" << LL_ENDL;
    bumpMeta();
    return true;
}

uint64_t LLFxRuntimeVK::budgetBytes(const Frame& f) const
{
    uint64_t cap = 0;
    static LLCachedControl<U32> s_cap(gSavedSettings, "SLFxVramBudgetMB", 1024);
    cap = (uint64_t)(U32)s_cap << 20;
    // Never past the live VRAM budget's free room either (soft: the effect just doesn't load).
    if (f.vramBudget > 0)
    {
        const uint64_t room = f.vramBudget > f.vramUsage ? f.vramBudget - f.vramUsage : 0;
        cap = cap ? std::min(cap, room) : room;
    }
    return cap;
}

// The effect's GPU memory (textures, sets, UBO) without its pipelines: it goes back to
// "compiled" and the next ensureLive rebuilds them (VRAM pressure parking).
static void releasePermResources(VkDeviceContext* ctx, LLFxRuntimeVK::Perm& p)
{
    using Perm = LLFxRuntimeVK::Perm;
    VkDevice dev = ctx->device();
    for (Perm::TexVK& t : p.tex)
    {
        for (VkImageView& v : t.mipViews) if (v) ctx->destroyView(v);
        t.mipViews.clear();
        if (t.srgbView) ctx->destroyView(t.srgbView);
        if (t.rtSrgb) ctx->destroyView(t.rtSrgb);
        if (t.img.image) ctx->destroyImage(t.img);
        t.img = GpuImage{};
    }
    for (Perm::TechVK& t : p.techs)
        for (Perm::PassVK& pv : t.passes) pv.set1 = pv.set2 = VK_NULL_HANDLE;
    if (p.pool) vkDestroyDescriptorPool(dev, p.pool, nullptr);
    p.pool = VK_NULL_HANDLE; p.set0 = VK_NULL_HANDLE;
    if (p.ubo.buffer) ctx->destroyBuffer(p.ubo);
    p.ubo = GpuBuffer{};
    p.samplers.clear();
    p.vramBytes = 0;
    p.uboFrame = ~0ull;
    if (p.state == 3) p.state = 2;
}

// SOFT VRAM PRESSURE (#546, our pressure-degrade rules): while the budget manager is in
// its reserve (mode 2), park the most expensive live effect every ~2 s (its textures are
// freed; it is skipped); after ~10 s calm (mode 0) give one back at a time. Temporary:
// no setting is written, the user's choices are untouched.
void LLFxRuntimeVK::applyPressure(const Frame& f)
{
    if (f.offscreen) return;
    if (f.vramMode >= 2)
    {
        mCalmFrames = 0;
        if (mFrameCounter % 120 != 0) return;
        Perm* worst = nullptr;
        for (auto& kv : mPerms)
            if (kv.second->state == 3 && !mParked.count(kv.second->effect) && (!worst || kv.second->vramBytes > worst->vramBytes))
                worst = kv.second.get();
        if (!worst || worst->vramBytes < (8ull << 20)) return;   // nothing worth parking
        vkDeviceWaitIdle(mCtx->device());
        const std::string eff = worst->effect;
        LL_WARNS("FX") << "video memory pressure: parking " << fileName(eff) << " (" << (worst->vramBytes >> 20)
                       << " MB) until it clears" << LL_ENDL;
        for (auto& kv : mPerms) if (kv.second->effect == eff && kv.second->state == 3) releasePermResources(mCtx, *kv.second);
        mParked[eff] = (uint32_t)mFrameCounter;
        std::lock_guard<std::mutex> lk(mStatusMutex);
        mStatus[eff].parked = true;
        mStatus[eff].vramBytes = 0;
        bumpMeta();
    }
    else if (f.vramMode == 0 && !mParked.empty())
    {
        if (++mCalmFrames < 600) return;
        mCalmFrames = 300;   // the next one after ~5 s more
        const std::string eff = mParked.begin()->first;
        mParked.erase(mParked.begin());
        LL_INFOS("FX") << "video memory pressure cleared: " << fileName(eff) << " back on" << LL_ENDL;
        std::lock_guard<std::mutex> lk(mStatusMutex);
        mStatus[eff].parked = false;
        bumpMeta();
    }
    else
        mCalmFrames = 0;
}

// ============================================================================
// Uniforms
// ============================================================================
void LLFxRuntimeVK::writeUniforms(Perm& p, const Frame& f, Shared& s)
{
    if (p.uboFrame == mFrameCounter) return;
    p.uboFrame = mFrameCounter;
    const reshadefx::effect_module& m = p.cr->module;
    std::vector<uint8_t>& d = p.uboData;
    std::fill(d.begin(), d.end(), 0);
    auto vit = mValues.find(p.effect);
    const LLSD* values = vit != mValues.end() ? vit->second.get() : nullptr;

    static const auto t0 = std::chrono::steady_clock::now();
    static auto tPrev = t0;
    static uint64_t tFrame = ~0ull;
    static float frameMs = 16.f;
    if (tFrame != mFrameCounter)
    {
        const auto now = std::chrono::steady_clock::now();
        frameMs = std::chrono::duration<float, std::milli>(now - tPrev).count();
        tPrev = now;
        tFrame = mFrameCounter;
    }
    const float timerMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static std::mt19937 rng(1234u);

    for (size_t ui = 0; ui < m.uniforms.size(); ++ui)
    {
        const reshadefx::uniform& u = m.uniforms[ui];
        if (u.offset + u.size > d.size()) continue;
        uint8_t* dst = d.data() + u.offset;
        const uint32_t comps = std::max(1u, (uint32_t)u.type.rows * std::max(1u, (uint32_t)u.type.cols));
        const bool isFloat = u.type.is_floating_point();
        const bool isBool = u.type.is_boolean();
        auto put = [&](uint32_t c, double v) {
            if (c >= comps || (c + 1) * 4 > u.size) return;
            if (isFloat) { float x = (float)v; std::memcpy(dst + c * 4, &x, 4); }
            else if (isBool) { uint32_t x = v != 0.0 ? 1u : 0u; std::memcpy(dst + c * 4, &x, 4); }
            else if (u.type.is_signed()) { int32_t x = (int32_t)v; std::memcpy(dst + c * 4, &x, 4); }
            else { uint32_t x = (uint32_t)std::max(0.0, v); std::memcpy(dst + c * 4, &x, 4); }
        };
        // Defaults first (initializer), raw, as ReShade stores them.
        if (u.has_initializer_value)
        {
            if (u.type.is_array())
            {
                for (size_t e = 0; e < u.initializer_value.array_data.size() && (e + 1) * 16 <= u.size; ++e)
                    std::memcpy(dst + e * 16, u.initializer_value.array_data[e].as_uint, std::min<size_t>(16, comps * 4));
            }
            else
                std::memcpy(dst, u.initializer_value.as_uint, std::min<size_t>(u.size, 64));
        }
        const std::string source = slfx::annotationString(u.annotations, "source");
        if (source.empty())
        {
            if (values && values->has(u.name))
            {
                const LLSD& v = (*values)[u.name];
                if (v.isArray()) for (uint32_t c = 0; c < (uint32_t)v.size(); ++c) put(c, v[(S32)c].asReal());
                else put(0, v.isBoolean() ? (v.asBoolean() ? 1.0 : 0.0) : v.asReal());
            }
            continue;
        }
        const float mn = slfx::annotationFloat(u.annotations, "min", 0.f);
        const float mx = slfx::annotationFloat(u.annotations, "max", 1.f);
        if (source == "timer") put(0, timerMs);
        else if (source == "frametime") put(0, frameMs);
        else if (source == "framecount") put(0, (double)(f.frameNumber & 0x7FFFFFFF));
        else if (source == "date")
        {
            const std::time_t tt = std::time(nullptr);
            std::tm lt{};
#if defined(_WIN32)
            localtime_s(&lt, &tt);
#else
            localtime_r(&tt, &lt);
#endif
            put(0, lt.tm_year + 1900); put(1, lt.tm_mon + 1); put(2, lt.tm_mday);
            put(3, lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
        }
        else if (source == "random")
        {
            const int lo = slfx::annotationInt(u.annotations, "min", 0), hi = slfx::annotationInt(u.annotations, "max", 32767);
            std::uniform_int_distribution<int> dist(std::min(lo, hi), std::max(lo, hi));
            put(0, dist(rng));
        }
        else if (source == "pingpong")
        {
            float& v = p.pingpong[ui * 2];
            float& dir = p.pingpong[ui * 2 + 1];
            const float step = slfx::annotationFloat(u.annotations, "step", 1.f);
            v += dir * step * frameMs * 0.001f;
            if (v >= mx) { v = mx; dir = -1.f; }
            else if (v <= mn) { v = mn; dir = 1.f; }
            put(0, v); put(1, dir);
        }
        else if (source == "key")
        {
            const int code = slfx::annotationInt(u.annotations, "keycode", 0);
            const bool down = code > 0 && mInputs.keyDown && mInputs.keyDown(code);
            const std::string mode = slfx::annotationString(u.annotations, "mode");
            if (down && !p.keyPrev[ui]) p.keyToggle[ui] ^= 1;
            const bool pressed = down && !p.keyPrev[ui];
            p.keyPrev[ui] = down;
            put(0, mode == "toggle" ? p.keyToggle[ui] : mode == "press" ? (pressed ? 1 : 0) : (down ? 1 : 0));
        }
        else if (source == "mousepoint")
        {
            put(0, mInputs.mouseX * (float)p.w / std::max(1.f, mInputs.windowW));
            put(1, mInputs.mouseY * (float)p.h / std::max(1.f, mInputs.windowH));
        }
        else if (source == "mousebutton")
        {
            const int code = std::clamp(slfx::annotationInt(u.annotations, "keycode", 0), 0, 4);
            const bool down = mInputs.mouseDown[code];
            const std::string mode = slfx::annotationString(u.annotations, "mode");
            if (down && !p.keyPrev[ui]) p.keyToggle[ui] ^= 1;
            const bool pressed = down && !p.keyPrev[ui];
            p.keyPrev[ui] = down;
            put(0, mode == "toggle" ? p.keyToggle[ui] : mode == "press" ? (pressed ? 1 : 0) : (down ? 1 : 0));
        }
        else if (source == "mousewheel")
        {
            float& v = p.pingpong[ui * 2];
            v = std::clamp(v + mInputs.wheelDelta, mn, std::max(mn, slfx::annotationFloat(u.annotations, "max", 1e9f)));
            put(0, v); put(1, mInputs.wheelDelta);
        }
        else if (source == "bufready_depth") put(0, 1.0);
        else if (source == "screenshot") put(0, f.captureFrame ? 1.0 : 0.0);
        // overlay_open / overlay_active / overlay_hovered / mousedelta / others: 0
    }
    std::memcpy((uint8_t*)p.ubo.mapped + (size_t)std::min(f.ringSlot, kRingMax - 1) * p.uboStride, d.data(), d.size());
}

// ============================================================================
// Input resolves
// ============================================================================
void LLFxRuntimeVK::resolveInputs(VkCommandBuffer cmd, const Frame& f, Shared& s, uint32_t needMask)
{
    if (s.resolvedFrame == mFrameCounter) needMask &= ~s.resolvedMask;
    else { s.resolvedFrame = mFrameCounter; s.resolvedMask = 0; }
    if (!needMask) return;
    VkDevice dev = mCtx->device();
    const uint32_t slot = std::min(f.ringSlot, kRingMax - 1);
    struct Push { int32_t dsize[2], rsize[2]; float ratio[2], jitter[2]; float lin[4]; float misc[4]; float misc2[4]; } pc{};
    static_assert(sizeof(Push) == 80, "fx_resolve.comp Push");
    pc.dsize[0] = (int32_t)s.w; pc.dsize[1] = (int32_t)s.h;
    pc.rsize[0] = (int32_t)std::max(1u, f.rw); pc.rsize[1] = (int32_t)std::max(1u, f.rh);
    pc.ratio[0] = (float)f.rw / (float)std::max(1u, s.w); pc.ratio[1] = (float)f.rh / (float)std::max(1u, s.h);
    pc.jitter[0] = f.jitter[0]; pc.jitter[1] = f.jitter[1];
    std::memcpy(pc.lin, f.lin, sizeof(pc.lin));
    pc.misc[0] = mFarPlane;
    pc.misc[2] = f.exposureConst;
    pc.misc[3] = f.unproj[0];
    pc.misc2[0] = f.unproj[1];

    // Every resolve variant binds the world depth (binding 1) in DEPTH_READ_ONLY: transition
    // it whenever anything dispatches (one barrier pair).
    const bool needDepthImg = f.depthImage != VK_NULL_HANDLE;
    if (needDepthImg)
        imageBarrier(cmd, f.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrierAll(cmd);   // prior reads of the input images (last frame / earlier point) -> these writes
    VkImageView mask = VK_NULL_HANDLE;
    if (needMask & (1u << IN_MASK))
    {
        std::lock_guard<std::mutex> lk(sMaskMutex);
        if (sMaskProvider) mask = sMaskProvider();
    }
    for (int k = 0; k < IN_COUNT; ++k)
    {
        if (!(needMask & (1u << k)) || !s.input[k].image) continue;
        VkImageView in = VK_NULL_HANDLE;
        bool zero = false;
        switch (k)
        {
        case IN_DEPTH: in = f.depthView; break;
        case IN_NORMAL: in = f.normalView ? f.normalView : f.depthView; pc.misc[1] = f.normalView ? 1.f : 0.f; break;
        case IN_MOTION: in = f.motionView; zero = !in; break;
        case IN_MASK: in = mask; zero = !in; break;
        case IN_HDR: in = f.hdrView; zero = !in; pc.misc[1] = f.exposureDyn && f.exposureView ? 1.f : 0.f; break;
        }
        if (zero || !in || !f.depthView)
        {
            VkClearColorValue z{};
            const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, s.input[k].image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
            continue;
        }
        VkDescriptorSet set = s.resolveSet[k][slot];
        const VkDescriptorImageInfo ii[4] = {
            {mHost->nearest, in, (k == IN_DEPTH || (k == IN_NORMAL && !f.normalView)) ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {mHost->nearest, f.depthView, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, s.input[k].view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, f.exposureView ? f.exposureView : mHost->dummyR32F.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = i;
            w[i].descriptorCount = 1; w[i].pImageInfo = &ii[i];
            w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mHost->resolve[k]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mHost->resolveLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, mHost->resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (s.w + 7) / 8, (s.h + 7) / 8, 1);
    }
    barrierAll(cmd);
    if (needDepthImg)
        imageBarrier(cmd, f.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    s.resolvedMask |= needMask;
}

// ============================================================================
// Recording
// ============================================================================
void LLFxRuntimeVK::readTimers(uint32_t slot)
{
    if (!mQueryPool[slot] || !mTimerIssued[slot]) return;
    const uint32_t n = (uint32_t)mTimedTechs[slot].size();
    if (!n) return;
    std::vector<uint64_t> data(n * 2 * 2, 0);
    const VkResult r = vkGetQueryPoolResults(mCtx->device(), mQueryPool[slot], 0, n * 2, data.size() * 8, data.data(),
                                             16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) return;
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint64_t* a = &data[i * 4];   // [ts0, avail0, ts1, avail1]
        if (!a[1] || !a[3] || a[2] < a[0]) continue;
        const float ms = (float)((double)(a[2] - a[0]) * mTimestampPeriod * 1e-6);
        const std::string key = mTimedTechs[slot][i].first + "\n" + mTimedTechs[slot][i].second;
        float& v = mGpuMs[key];
        v = v <= 0.f ? ms : v * 0.9f + ms * 0.1f;
    }
}

void LLFxRuntimeVK::recordTechnique(VkCommandBuffer cmd, const Frame& f, Perm& p, Shared& s, size_t techIndex,
                                    uint8_t scope, uint8_t feather)
{
    const reshadefx::effect_module& m = p.cr->module;
    const reshadefx::technique& t = m.techniques[techIndex];
    Perm::TechVK& tv = p.techs[techIndex];
    VkDevice dev = mCtx->device();
    // Per-effect scope (#548): keep the image before the technique, blend it back by the
    // avatar mask after (runPoint made scopeSave, the mask and the set).
    VkPipeline scopePipe = VK_NULL_HANDLE;
    if (scope != SCOPE_ALL && s.scopeSave.image && s.scopeSetWritten)
        scopePipe = cachedPipe(mHost->scopePipes, s.bbFormat, [&] {
            return hostGraphicsPipe(dev, mHost->vs, mHost->scopeFs, mHost->scopeLayout, s.bbFormat);
        });
    VkImageCopy whole{};
    whole.srcSubresource = whole.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    whole.extent = {s.w, s.h, 1};
    if (scopePipe)
    {
        barrierAll(cmd);
        vkCmdCopyImage(cmd, s.bbRT.image, VK_IMAGE_LAYOUT_GENERAL, s.scopeSave.image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    }

    if (tv.usesStencil && s.stencil.image)
    {
        VkClearDepthStencilValue z{0.f, 0};
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_STENCIL_BIT | (s.stencilFmt == VK_FORMAT_S8_UINT ? 0u : VK_IMAGE_ASPECT_DEPTH_BIT),
                                        0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, s.stencil.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    }
    const uint32_t dynOff = std::min(f.ringSlot, kRingMax - 1) * p.uboStride;
    for (size_t pi = 0; pi < tv.passes.size(); ++pi)
    {
        const reshadefx::pass& ps = t.passes[pi];
        Perm::PassVK& pv = tv.passes[pi];
        barrierAll(cmd);
        const VkDescriptorSet sets[3] = {p.set0, pv.set1, pv.set2};
        if (pv.compute)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pv.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pv.layout, 0, 3, sets, 1, &dynOff);
            vkCmdDispatch(cmd, std::max(1u, pv.vpW), std::max(1u, pv.vpH), std::max(1u, pv.vpZ));
        }
        else
        {
            VkRenderingAttachmentInfo ca[8]{};
            for (uint32_t k = 0; k < pv.rtCount; ++k)
            {
                VkImageView v;
                if (pv.rt[k] < 0) v = (pv.srgbWrite && s.bbRTSrgb) ? s.bbRTSrgb : s.bbRT.view;
                else
                {
                    const Perm::TexVK& tt = p.tex[pv.rt[k]];
                    v = (pv.srgbWrite && tt.rtSrgb) ? tt.rtSrgb : tt.mipViews[0];
                }
                ca[k].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ca[k].imageView = v; ca[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ca[k].loadOp = ps.clear_render_targets ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                ca[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            VkRenderingAttachmentInfo sa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {pv.vpW, pv.vpH}}; ri.layerCount = 1;
            ri.colorAttachmentCount = pv.rtCount; ri.pColorAttachments = ca;
            if (pv.stencil)
            {
                if (!s.stencil.image) continue;   // (allocated at go-live when any pass needs it)
                sa.imageView = s.stencil.view; sa.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ri.pStencilAttachment = &sa;
            }
            vkCmdBeginRendering(cmd, &ri);
            // ReShade's Vulkan convention: negative-height viewport (no flip in the shaders).
            VkViewport vp{0.f, (float)pv.vpH, (float)pv.vpW, -(float)pv.vpH, 0.f, 1.f};
            VkRect2D sc{{0, 0}, {pv.vpW, pv.vpH}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pv.pipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pv.layout, 0, 3, sets, 1, &dynOff);
            vkCmdDraw(cmd, std::max(1u, ps.num_vertices), 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
        barrierAll(cmd);
        // The back buffer changed: refresh the sampled copy (BackBuffer reads see it).
        if (pv.toBackBuffer)
        {
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.extent = {s.w, s.h, 1};
            vkCmdCopyImage(cmd, s.bbRT.image, VK_IMAGE_LAYOUT_GENERAL, s.bbTex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
        // Mipmaps of written render targets / storages.
        if (ps.generate_mipmaps)
        {
            std::vector<int> written;
            for (uint32_t k = 0; k < pv.rtCount; ++k) if (pv.rt[k] >= 0) written.push_back(pv.rt[k]);
            for (const reshadefx::storage_binding& ub : ps.storage_bindings)
            {
                const std::string& tn = m.storages[ub.index].texture_name;
                for (size_t i = 0; i < m.textures.size(); ++i) if (m.textures[i].unique_name == tn) written.push_back((int)i);
            }
            for (int ti : written)
            {
                Perm::TexVK& tt = p.tex[ti];
                if (tt.levels < 2 || !tt.linearBlit) continue;
                for (uint32_t l = 1; l < tt.levels; ++l)
                {
                    barrierAll(cmd);
                    VkImageBlit b{};
                    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
                    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                    b.srcOffsets[1] = {(int32_t)std::max(1u, tt.w >> (l - 1)), (int32_t)std::max(1u, tt.h >> (l - 1)), 1};
                    b.dstOffsets[1] = {(int32_t)std::max(1u, tt.w >> l), (int32_t)std::max(1u, tt.h >> l), 1};
                    vkCmdBlitImage(cmd, tt.img.image, VK_IMAGE_LAYOUT_GENERAL, tt.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
                }
            }
        }
    }
    barrierAll(cmd);
    if (scopePipe)
    {
        // bbTex = the technique's result; composite (pre, post, mask) -> the back buffer.
        vkCmdCopyImage(cmd, s.bbRT.image, VK_IMAGE_LAYOUT_GENERAL, s.bbTex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
        barrierAll(cmd);
        const int32_t push[4] = {(int32_t)scope, (int32_t)feather, 0, 0};
        drawFullscreen(cmd, s.bbRT.view, VK_IMAGE_LAYOUT_GENERAL, s.w, s.h, scopePipe, mHost->scopeLayout, s.scopeSet,
                       push, sizeof(push));
        barrierAll(cmd);
        vkCmdCopyImage(cmd, s.bbRT.image, VK_IMAGE_LAYOUT_GENERAL, s.bbTex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
        barrierAll(cmd);
    }
}

bool LLFxRuntimeVK::runPoint(VkCommandBuffer cmd, const Frame& f, uint8_t point, Shared& s)
{
    // Collect this point's live techniques in run order.
    struct Run { std::shared_ptr<Perm> p; size_t ti; uint8_t scope, feather; };
    std::vector<Run> runs;
    uint32_t need = 0;
    const VkFormat bbFmt = s.bbFormat;
    const bool sync = f.offscreen;   // a snapshot cannot wait for the worker
    for (const PlanTech& pt : mPlan)
    {
        if (techPoint(pt) != point) continue;
        if (mParked.count(pt.effect)) continue;   // VRAM pressure (#546)
        std::shared_ptr<Perm> p = findOrQueue(pt.effect, s.w, s.h, point, f.colorFormat, sync);
        if (!p) continue;
        if (sync) pollJobs();
        if (p->state == -1 || p->state < 2 || p->bbFormat != bbFmt) continue;
        std::string err;
        if (p->state == 2)
        {
            if (!ensureLive(*p, s, f, err))
            {
                if (!err.empty())
                {
                    vkDeviceWaitIdle(mCtx->device());
                    if (f.offscreen)
                    {
                        // A snapshot-size failure (#547) says so and leaves the live view's
                        // effect alone: the shot goes without this effect.
                        releasePermResources(mCtx, *p);
                        mSnapNote += (mSnapNote.empty() ? "" : " ") + fileName(pt.effect) + " was left out of this snapshot: "
                                   + err.substr(err.find(':') == std::string::npos ? 0 : err.find(':') + 2) + ".";
                        LL_WARNS("FX") << mSnapNote << LL_ENDL;
                    }
                    else
                    {
                        destroyPerm(*p);
                        p->state = -1;
                        setStatusError(pt.effect, err);
                    }
                }
                continue;
            }
        }
        const reshadefx::effect_module& m = p->cr->module;
        for (size_t ti = 0; ti < m.techniques.size(); ++ti)
        {
            if (m.techniques[ti].name != pt.technique) continue;
            if (f.captureFrame && !p->techs[ti].enabledInScreenshot) continue;
            runs.push_back({p, ti, pt.scope, pt.feather});
            for (const Perm::TexVK& t : p->tex)
            {
                switch (t.sem)
                {
                case SEM_DEPTH: need |= 1u << IN_DEPTH; break;
                case SEM_NORMAL: need |= 1u << IN_NORMAL; break;
                case SEM_MOTION: need |= 1u << IN_MOTION; break;
                case SEM_MASK: need |= 1u << IN_MASK; break;
                case SEM_HDR: need |= 1u << IN_HDR; break;
                default: break;
                }
            }
            if (pt.scope != SCOPE_ALL) need |= 1u << IN_MASK;
        }
    }
    if (need & (1u << IN_NORMAL)) mWantNormals = true;
    mWantMask = (need & (1u << IN_MASK)) != 0;
    if (runs.empty()) return false;

    // Stencil, when a live technique needs it.
    bool wantStencil = false;
    for (const Run& r : runs) wantStencil = wantStencil || r.p->techs[r.ti].usesStencil;
    if (wantStencil && !s.stencil.image)
    {
        for (VkFormat c : {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT})
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(mCtx->physicalDevice(), c, &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { s.stencilFmt = c; break; }
        }
        if (s.stencilFmt != VK_FORMAT_UNDEFINED)
        {
            VkMemScope memcat(VKMEM_TARGET);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            const VkImageAspectFlags asp = VK_IMAGE_ASPECT_STENCIL_BIT | (s.stencilFmt == VK_FORMAT_S8_UINT ? 0u : VK_IMAGE_ASPECT_DEPTH_BIT);
            s.stencil = mCtx->createImage2D(s.w, s.h, s.stencilFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, asp);
            if (s.stencil.image)
            {
                s.vramBytes += mCtx->allocBytes(s.stencil);
                mCtx->submitImmediate([&](VkCommandBuffer c) {
                    imageBarrier(c, s.stencil.image, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
                });
            }
        }
    }

    // Per-effect scope (#548): the pre-technique copy + the mask, and their set (once).
    bool wantScope = false;
    for (const Run& r : runs) wantScope = wantScope || r.scope != SCOPE_ALL;
    if (wantScope && !s.scopeSetWritten && ensureSharedInput(s, IN_MASK))
    {
        if (!s.scopeSave.image)
        {
            VkMemScope memcat(VKMEM_TARGET);
            VkDeviceContext::SoftAllocScope soft(mCtx);
            s.scopeSave = mCtx->createImage2D(s.w, s.h, s.bbFormat,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            if (s.scopeSave.image)
            {
                s.vramBytes += mCtx->allocBytes(s.scopeSave);
                mCtx->setObjectName(VK_OBJECT_TYPE_IMAGE, (uint64_t)s.scopeSave.image, "fxScopeSave");
                mCtx->submitImmediate([&](VkCommandBuffer c) {
                    imageBarrier(c, s.scopeSave.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
                });
            }
        }
        if (s.scopeSave.image)
        {
            VkDevice dev = mCtx->device();
            writeImage(dev, s.scopeSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mHost->nearest, s.scopeSave.view);
            writeImage(dev, s.scopeSet, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mHost->nearest, s.bbTex.view);
            writeImage(dev, s.scopeSet, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mHost->nearest, s.input[IN_MASK].view);
            s.scopeSetWritten = true;
        }
    }

    resolveInputs(cmd, f, s, need);
    barrierAll(cmd);
    {
        VkImageCopy c{};
        c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {s.w, s.h, 1};
        vkCmdCopyImage(cmd, s.bbRT.image, VK_IMAGE_LAYOUT_GENERAL, s.bbTex.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }

    // GPU timers (live frames only): two timestamps per technique.
    const uint32_t slot = std::min(f.ringSlot, kRingMax - 1);
    static LLCachedControl<bool> s_timers(gSavedSettings, "SLFxGpuTimers", true);
    const bool timed = !f.offscreen && s_timers && mTimestampPeriod > 0.f;
    if (timed && !mQueryPool[slot])
    {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = kMaxTimedTechs * 2;
        if (vkCreateQueryPool(mCtx->device(), &qi, nullptr, &mQueryPool[slot]) == VK_SUCCESS)
            mCtx->submitImmediate([&](VkCommandBuffer c) { vkCmdResetQueryPool(c, mQueryPool[slot], 0, kMaxTimedTechs * 2); });
        else
            mQueryPool[slot] = VK_NULL_HANDLE;
    }

    std::string summary;
    for (size_t i = 0; i < runs.size(); ++i)
    {
        Run& r = runs[i];
        const std::string& tname = r.p->cr->module.techniques[r.ti].name;
        writeUniforms(*r.p, f, s);
        const uint32_t q = (uint32_t)mTimedTechs[slot].size();
        const bool timeThis = timed && mQueryPool[slot] && q < kMaxTimedTechs;
        if (timeThis)
        {
            mTimedTechs[slot].emplace_back(r.p->effect, tname);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueryPool[slot], q * 2);
        }
        vkBreadcrumb(cmd, kTechMarks[std::min<size_t>(i, 63)]);
        recordTechnique(cmd, f, *r.p, s, r.ti, r.scope, r.feather);
        if (timeThis) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueryPool[slot], q * 2 + 1);
        if (!f.offscreen) ++r.p->goodFrames;
        r.p->lastUsed = mFrameCounter;
        summary += (summary.empty() ? "" : " ") + tname + "@" + fileName(r.p->effect);
        if (std::find(mActive.begin(), mActive.end(), r.p->effect) == mActive.end()) mActive.push_back(r.p->effect);
    }
    if (timed) mTimerIssued[slot] = true;
    {
        std::lock_guard<std::mutex> lk(sLastMutex);
        sLastRecorded = summary;
    }
    return true;
}

// HDR injection point (#548): mHdr (linear, pre-exposure) -> x exposure -> our RGBA16F
// back buffer -> the HDR-point techniques -> / exposure -> mHdr. mHdr is
// SHADER_READ_ONLY_OPTIMAL in and out (bloom / DoF / the tonemapper sample it).
bool LLFxRuntimeVK::recordHdr(VkCommandBuffer cmd, const Frame& f)
{
    if (!mEnabled || mSuppress > 0 || mPlan.empty() || !f.ctx || f.skip || !f.hdrImage || !f.hdrView
        || f.width == 0 || f.height == 0)
        return false;
    attachDevice(f.ctx);
    pollJobs();
    bool any = false;
    for (const PlanTech& pt : mPlan) any = any || techPoint(pt) == POINT_HDR;
    if (!any || !initHost()) return false;
    const VkFormat hdrBb = VK_FORMAT_R16G16B16A16_SFLOAT;
    // Anything compiled yet? (Queues the compiles otherwise; offscreen compiles inline.)
    bool ready = false;
    for (const PlanTech& pt : mPlan)
    {
        if (techPoint(pt) != POINT_HDR || mParked.count(pt.effect)) continue;
        std::shared_ptr<Perm> p = findOrQueue(pt.effect, f.width, f.height, POINT_HDR, f.colorFormat, f.offscreen);
        if (f.offscreen) pollJobs();
        ready = ready || (p && p->state >= 2);
    }
    if (!ready) return false;
    Shared* s = ensureShared(f.width, f.height, POINT_HDR, hdrBb);
    if (!s) return false;
    frameStart(cmd, f);
    VkDevice dev = mCtx->device();
    const uint32_t slot = std::min(f.ringSlot, kRingMax - 1);
    VkPipeline inPipe = cachedPipe(mHost->hdrInPipes, hdrBb, [&] {
        return hostGraphicsPipe(dev, mHost->vs, mHost->hdrInFs, mHost->hdrLayout, hdrBb);
    });
    VkPipeline outPipe = cachedPipe(mHost->hdrOutPipes, f.hdrFormat, [&] {
        return hostGraphicsPipe(dev, mHost->vs, mHost->hdrOutFs, mHost->hdrLayout, f.hdrFormat);
    });
    if (!inPipe || !outPipe) return false;
    const VkImageView expo = f.exposureView ? f.exposureView : mHost->dummyR32F.view;
    const float push[4] = {f.exposureConst, (f.exposureDyn && f.exposureView) ? 1.f : 0.f, 0.f, 0.f};
    // In: the renderer's views -> per-slot set (rewritten each use; target generation rule).
    {
        VkDescriptorImageInfo ii{mHost->nearest, f.hdrView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s->hdrInSet[slot]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        writeImage(dev, s->hdrInSet[slot], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, expo);
        writeImage(dev, s->hdrOutSet[slot], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mHost->nearest, s->bbRT.view);
        writeImage(dev, s->hdrOutSet[slot], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_NULL_HANDLE, expo);
    }
    barrierAll(cmd);   // the previous frame's reads of our back buffer -> this write
    drawFullscreen(cmd, s->bbRT.view, VK_IMAGE_LAYOUT_GENERAL, s->w, s->h, inPipe, mHost->hdrLayout,
                   s->hdrInSet[slot], push, sizeof(push));
    barrierAll(cmd);
    const bool ran = runPoint(cmd, f, POINT_HDR, *s);
    // Out: our result (/ exposure) over mHdr.
    imageBarrier(cmd, f.hdrImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    drawFullscreen(cmd, f.hdrView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, s->w, s->h, outPipe, mHost->hdrLayout,
                   s->hdrOutSet[slot], push, sizeof(push));
    imageBarrier(cmd, f.hdrImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    return ran;
}

bool LLFxRuntimeVK::wantsDisplay(const Frame& f)
{
    if (!mEnabled || mSuppress > 0 || mPlan.empty() || !f.ctx || f.width == 0 || f.height == 0) return false;
    attachDevice(f.ctx);
    pollJobs();
    if (!f.offscreen)
    {
        // Far plane (a compile-time define): rounded up to 64 m so draw-distance nudges
        // don't recompile every effect.
        const float fp = std::max(64.f, std::ceil(mInputs.farClip / 64.f) * 64.f);
        if (fp != mFarPlane) mFarPlane = fp;
        mMainW = f.width; mMainH = f.height; mMainFormat = f.colorFormat;
    }
    // Snapshots (#547): only the recording that ends in the capture runs effects (the
    // warm-up and the first N-1 supersample recordings would be N times the cost).
    if (f.skip) return false;
    if (f.offscreen && f.captureFrame) mSnapNote.clear();
    // Any planned technique with a usable (or soon usable) permutation at this size?
    // Offscreen: compiled synchronously (BUFFER_WIDTH / HEIGHT are compile-time; the shot
    // cannot wait), then a VRAM pre-check for the whole set at the snapshot size.
    bool any = false;
    uint64_t need = 0;
    for (const PlanTech& pt : mPlan)
    {
        if (mParked.count(pt.effect)) continue;
        if (techPoint(pt) != POINT_DISPLAY) continue;   // HDR-point techniques: recordHdr
        std::shared_ptr<Perm> p = findOrQueue(pt.effect, f.width, f.height, POINT_DISPLAY, f.colorFormat, f.offscreen);
        if (f.offscreen) pollJobs();
        if (!p || p->state < 2) continue;
        any = true;
        if (f.offscreen && p->state == 2 && p->cr)
            for (size_t i = 0; i < p->tex.size(); ++i)
                if (p->tex[i].sem == SEM_NONE)
                    need += mCtx->imageMemoryBytes(p->tex[i].w, p->tex[i].h, 1, p->tex[i].levels, p->tex[i].fmt,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                   false);
    }
    if (any && f.offscreen)
    {
        // + the shared back buffer pair and the inputs (upper bound: 2 x RGBA8 + 5 inputs).
        need += (uint64_t)f.width * f.height * (8 + 4 + 8 + 4 + 4 + 8);
        const uint64_t room = f.vramBudget > f.vramUsage ? f.vramBudget - f.vramUsage : 0;
        if (f.vramBudget && need > room)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "Effects were left out of this %ux%u snapshot: they need ~%llu MB of video memory, ~%llu MB free.",
                          f.width, f.height, (unsigned long long)(need >> 20), (unsigned long long)(room >> 20));
            mSnapNote = buf;
            LL_WARNS("FX") << mSnapNote << LL_ENDL;
            return false;
        }
    }
    return any;
}

// Once per recording, by whichever point runs first: frame counter, timer slot.
void LLFxRuntimeVK::frameStart(VkCommandBuffer cmd, const Frame& f)
{
    if (mStartedRecording == f.frameNumber && mStartedOffscreen == f.offscreen && !f.offscreen) return;
    mStartedRecording = f.frameNumber;
    mStartedOffscreen = f.offscreen;
    ++mFrameCounter;
    mActive.clear();
    const uint32_t slot = std::min(f.ringSlot, kRingMax - 1);
    if (!f.offscreen)
    {
        readTimers(slot);
        mTimedTechs[slot].clear();
        mTimerIssued[slot] = false;
        if (mQueryPool[slot]) vkCmdResetQueryPool(cmd, mQueryPool[slot], 0, kMaxTimedTechs * 2);
    }
}

uint8_t LLFxRuntimeVK::techPoint(const PlanTech& pt) const
{
    auto cfg = mConfigs.find(pt.effect);
    if (cfg != mConfigs.end() && cfg->second.point == POINT_HDR) return POINT_HDR;
    // A technique may ask for the HDR point itself (annotation slipstream_point = "hdr").
    std::lock_guard<std::mutex> lk(mStatusMutex);
    auto st = mStatus.find(pt.effect);
    if (st != mStatus.end() && st->second.meta)
        for (const reshadefx::technique& t : st->second.meta->module.techniques)
            if (t.name == pt.technique)
                return slfx::annotationString(t.annotations, "slipstream_point") == "hdr" ? (uint8_t)POINT_HDR
                                                                                         : (uint8_t)POINT_DISPLAY;
    return POINT_DISPLAY;
}

VkImageView LLFxRuntimeVK::beginDisplay(VkCommandBuffer cmd, const Frame& f)
{
    frameStart(cmd, f);
    Shared* s = ensureShared(f.width, f.height, POINT_DISPLAY, f.colorFormat);
    if (!s) return VK_NULL_HANDLE;
    // Contents discarded; waits on the previous frame's FX reads of it.
    imageBarrier(cmd, s->bbRT.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    return s->bbRT.view;
}

void LLFxRuntimeVK::recordDisplay(VkCommandBuffer cmd, const Frame& f, VkImageView dstView)
{
    Shared* s = ensureShared(f.width, f.height, POINT_DISPLAY, f.colorFormat);
    if (!s || !mHost) return;
    imageBarrier(cmd, s->bbRT.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                 VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    applyPressure(f);
    runPoint(cmd, f, POINT_DISPLAY, *s);
    // The result -> the chain's own image (same size and format), COLOR_ATTACHMENT in/out.
    VkPipeline pipe = cachedPipe(mHost->blitPipes, f.colorFormat, [&] {
        return hostGraphicsPipe(mCtx->device(), mHost->vs, mHost->blitFs, mHost->blitLayout, f.colorFormat);
    });
    barrierAll(cmd);
    if (pipe)
        drawFullscreen(cmd, dstView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, f.width, f.height, pipe,
                       mHost->blitLayout, s->blitSet, nullptr, 0);
    barrierAll(cmd);

    // Housekeeping: free size entries / perms nobody used for a while.
    if (!f.offscreen && (mFrameCounter % 120) == 0)
    {
        bool waited = false;
        for (auto it = mPerms.begin(); it != mPerms.end();)
        {
            Perm& p = *it->second;
            // Live ones unused for a while, and compiled / failed ones of other sizes
            // (resizes, warmed snapshot sizes): no unbounded growth of the cache.
            const bool otherSize = p.w != mMainW || p.h != mMainH;
            if (p.state != 1 && mFrameCounter - p.lastUsed > kIdleFreeFrames && (p.state == 3 || otherSize))
            {
                if (!waited) { vkDeviceWaitIdle(mCtx->device()); waited = true; }
                destroyPerm(p);
                it = mPerms.erase(it);
            }
            else
                ++it;
        }
        for (auto it = mShared.begin(); it != mShared.end();)
        {
            if (mFrameCounter - (*it)->lastUsed > kIdleFreeFrames)
            {
                if (!waited) { vkDeviceWaitIdle(mCtx->device()); waited = true; }
                destroyShared(**it);
                it = mShared.erase(it);
            }
            else
                ++it;
        }
        std::lock_guard<std::mutex> lk(mStatusMutex);
        for (auto& kv : mStatus)
        {
            uint32_t good = 0;
            for (auto& pk : mPerms) if (pk.second->effect == kv.first) good = std::max(good, pk.second->goodFrames);
            kv.second.goodFrames = good;
        }
    }
}

void LLFxRuntimeVK::onEndOffscreen()
{
    if (!mCtx) return;
    vkDeviceWaitIdle(mCtx->device());
    for (auto it = mPerms.begin(); it != mPerms.end();)
    {
        Perm& p = *it->second;
        if ((p.w != mMainW || p.h != mMainH) && p.state != 1) { destroyPerm(p); it = mPerms.erase(it); }
        else ++it;
    }
    for (auto it = mShared.begin(); it != mShared.end();)
    {
        if ((*it)->w != mMainW || (*it)->h != mMainH) { destroyShared(**it); it = mShared.erase(it); }
        else ++it;
    }
}
