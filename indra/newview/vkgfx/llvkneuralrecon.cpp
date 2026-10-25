/**
 * @file llvkneuralrecon.cpp
 * @brief The run-time-loaded slip_nr library (#870, #962): see llvkneuralrecon.h.
 */
#include "linden_common.h"

#include "llvkneuralrecon.h"

#define SLIP_NR_NO_PROTOTYPES 1
#include "slipstream_ngx.h"   // contrib/ngxshim/include (MIT; the ABI, no vendor code)

#include "llerror.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

struct LLVKNeuralRecon::Fns
{
    PFN_slip_nr_abi_version         abiVersion = nullptr;
    PFN_slip_nr_get_info            getInfo = nullptr;
    PFN_slip_nr_instance_extensions instanceExtensions = nullptr;
    PFN_slip_nr_device_extensions   deviceExtensions = nullptr;
    PFN_slip_nr_init                init = nullptr;
    PFN_slip_nr_get_caps            getCaps = nullptr;
    PFN_slip_nr_optimal_render_size optimalRenderSize = nullptr;
    PFN_slip_nr_optimal_render_size2 optimalRenderSize2 = nullptr;   // optional: shim 1.2+ (SR)
    PFN_slip_nr_create              create = nullptr;
    PFN_slip_nr_evaluate            evaluate = nullptr;
    PFN_slip_nr_destroy             destroy = nullptr;
    PFN_slip_nr_shutdown            shutdown = nullptr;
    PFN_slip_nr_last_error          lastError = nullptr;
};

struct LLVKNeuralRecon::Feature
{
    slip_nr_feature* f = nullptr;
};

namespace
{
std::string dirOf(const std::string& p)
{
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

bool isDir(const std::string& p)
{
#if LL_WINDOWS
    std::wstring w(p.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, &w[0], (int)w.size());
    if (n <= 0) return false;
    const DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

const char* libName()
{
#if LL_WINDOWS
    return SLIP_NR_LIBRARY_NAME_WINDOWS;
#else
    return SLIP_NR_LIBRARY_NAME_LINUX;
#endif
}

void* openLib(const std::string& path, std::string& err)
{
#if LL_WINDOWS
    std::wstring w(path.size() + 1, L'\0');
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], (int)w.size());
    if (n <= 0) { err = "bad path"; return nullptr; }
    w.resize((size_t)n - 1);
    // LOAD_WITH_ALTERED_SEARCH_PATH: the library's own folder resolves its dependencies.
    HMODULE h = LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "LoadLibrary error %lu", (unsigned long)GetLastError());
        err = buf;
    }
    return (void*)h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        const char* e = dlerror();
        err = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void closeLib(void* h)
{
    if (!h) return;
#if LL_WINDOWS
    FreeLibrary((HMODULE)h);
#else
    dlclose(h);
#endif
}

void SLIP_NR_CALL logThunk(void*, uint32_t level, const char* msg)
{
    LLVKNeuralRecon::logLine(level, msg);
}

std::string joinNames(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& e : v) s += (s.empty() ? "" : ", ") + e;
    return s.empty() ? std::string("none") : s;
}

slip_nr_image toImage(const LLVKNeuralRecon::Image& i)
{
    slip_nr_image o{};
    o.image = i.image;
    o.view = i.view;
    o.format = i.format;
    o.width = i.w;
    o.height = i.h;
    o.aspect = i.aspect;
    return o;
}
} // namespace

const char* LLVKNeuralRecon::qualityName(Quality q)
{
    switch (q)
    {
    case Q_DLAA:              return "DLAA";
    case Q_QUALITY:           return "Quality";
    case Q_BALANCED:          return "Balanced";
    case Q_PERFORMANCE:       return "Performance";
    case Q_ULTRA_PERFORMANCE: return "Ultra Performance";
    default:                  return "?";
    }
}

LLVKNeuralRecon& LLVKNeuralRecon::instance()
{
    static LLVKNeuralRecon s;
    return s;
}

void LLVKNeuralRecon::logLine(uint32_t level, const char* msg)
{
    if (!msg) return;
    std::string m(msg);
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (m.empty()) return;
    static std::mutex s_mutex;   // the vendor may log from its own threads
    std::lock_guard<std::mutex> lock(s_mutex);
    LL_DEBUGS("NGX") << (level ? "(verbose) " : "") << m << LL_ENDL;
}

void* LLVKNeuralRecon::sym(const char* name)
{
    if (!mLib) return nullptr;
#if LL_WINDOWS
    return (void*)GetProcAddress((HMODULE)mLib, name);
#else
    return dlsym(mLib, name);
#endif
}

void LLVKNeuralRecon::fail(const std::string& why)
{
    mAvailable = false;
    mFeatAvail[0] = mFeatAvail[1] = false;
    if (mReason.empty()) mReason = why;
}

void LLVKNeuralRecon::unload()
{
    closeLib(mLib);
    mLib = nullptr;
    delete mFn;
    mFn = nullptr;
}

void LLVKNeuralRecon::fillAppInfo(void* p)
{
    slip_nr_app_info& app = *(slip_nr_app_info*)p;
    app = slip_nr_app_info{};
    app.struct_size = sizeof(slip_nr_app_info);
    app.project_id = nullptr;   // the shim's own default id (custom engine)
    app.engine_version = mCfg.engineVersion.c_str();
    app.app_data_path = mCfg.dataDir.c_str();
    app.feature_paths = mFeaturePathPtrs.empty() ? nullptr : mFeaturePathPtrs.data();
    app.feature_path_count = (uint32_t)mFeaturePathPtrs.size();
    app.log_fn = logThunk;
    app.log_user = nullptr;
    app.log_verbose = 0;
}

bool LLVKNeuralRecon::load(const Config& cfg)
{
    if (mTried) return loaded();
    mTried = true;
    mCfg = cfg;
    mReason.clear();
    mAvailable = false;
    mFeatAvail[0] = mFeatAvail[1] = false;
    mFeatReason[0].clear();
    mFeatReason[1].clear();
    if (!cfg.enabled)
    {
        mReason = "disabled (RenderVKDlssShim 0)";
        finishStatus();
        return false;
    }
    // Candidates: the configured path (a folder or the file itself), then the viewer's libraries.
    std::vector<std::string> tries;
    if (!cfg.configuredPath.empty())
    {
        if (isDir(cfg.configuredPath)) tries.push_back(cfg.configuredPath + "/" + libName());
        else tries.push_back(cfg.configuredPath);
    }
    else if (!cfg.defaultDir.empty())
        tries.push_back(cfg.defaultDir + "/" + libName());   // RenderVKDlssShimPath empty: the default folder
    if (!cfg.viewerLibDir.empty()) tries.push_back(cfg.viewerLibDir + "/" + libName());
    std::string errs;
    for (const std::string& t : tries)
    {
        std::string err;
        void* h = openLib(t, err);
        if (h)
        {
            mLib = h;
            mLibPath = t;
            break;
        }
        LL_DEBUGS("Vulkan") << "DLSS: " << t << ": " << err << LL_ENDL;
        // (a plain "not found" is the normal case: only report other failures)
        if (err.find("No such file") == std::string::npos && err.find("error 126") == std::string::npos)
            errs += (errs.empty() ? "" : "; ") + t + ": " + err;
    }
    if (!mLib)
    {
        {
            std::string looked;
            for (const std::string& t : tries) looked += (looked.empty() ? "" : ", ") + t;
            LL_INFOS("Vulkan") << "DLSS: shim not found; looked in: " << looked
                               << (cfg.configuredPath.empty() ? " (RenderVKDlssShimPath is empty)" : "")
                               << ". Run contrib/ngxshim/build.sh to install it into " << cfg.defaultDir << LL_ENDL;
        }
        mReason = std::string("the user-installed shim (") + libName() + ") was not found"
                + (errs.empty() ? std::string() : " or failed to load (" + errs + ")");
        finishStatus();
        return false;
    }
    mFn = new Fns();
    mFn->abiVersion = (PFN_slip_nr_abi_version)sym(SLIP_NR_FN_ABI_VERSION);
    mFn->getInfo = (PFN_slip_nr_get_info)sym(SLIP_NR_FN_GET_INFO);
    mFn->instanceExtensions = (PFN_slip_nr_instance_extensions)sym(SLIP_NR_FN_INSTANCE_EXTENSIONS);
    mFn->deviceExtensions = (PFN_slip_nr_device_extensions)sym(SLIP_NR_FN_DEVICE_EXTENSIONS);
    mFn->init = (PFN_slip_nr_init)sym(SLIP_NR_FN_INIT);
    mFn->getCaps = (PFN_slip_nr_get_caps)sym(SLIP_NR_FN_GET_CAPS);
    mFn->optimalRenderSize = (PFN_slip_nr_optimal_render_size)sym(SLIP_NR_FN_OPTIMAL_RENDER_SIZE);
    mFn->optimalRenderSize2 = (PFN_slip_nr_optimal_render_size2)sym(SLIP_NR_FN_OPTIMAL_RENDER_SIZE2);   // (optional)
    mFn->create = (PFN_slip_nr_create)sym(SLIP_NR_FN_CREATE);
    mFn->evaluate = (PFN_slip_nr_evaluate)sym(SLIP_NR_FN_EVALUATE);
    mFn->destroy = (PFN_slip_nr_destroy)sym(SLIP_NR_FN_DESTROY);
    mFn->shutdown = (PFN_slip_nr_shutdown)sym(SLIP_NR_FN_SHUTDOWN);
    mFn->lastError = (PFN_slip_nr_last_error)sym(SLIP_NR_FN_LAST_ERROR);
    const bool all = mFn->abiVersion && mFn->getInfo && mFn->instanceExtensions && mFn->deviceExtensions && mFn->init
                  && mFn->getCaps && mFn->optimalRenderSize && mFn->create && mFn->evaluate && mFn->destroy
                  && mFn->shutdown && mFn->lastError;
    if (!all)
    {
        mReason = mLibPath + " is not a slip_nr library (entry points missing)";
        unload();
        finishStatus();
        return false;
    }
    const uint32_t abi = mFn->abiVersion();
    if (abi != SLIP_NR_ABI_VERSION)
    {
        mReason = mLibPath + ": ABI version " + std::to_string(abi) + ", this viewer needs "
                + std::to_string(SLIP_NR_ABI_VERSION) + " (rebuild the shim from this viewer's contrib/ngxshim)";
        unload();
        finishStatus();
        return false;
    }
    slip_nr_library_info li{};
    li.struct_size = sizeof(li);
    if (mFn->getInfo(&li) == SLIP_NR_OK)
    {
        mBackend = li.backend;
        mSdkVersion = li.sdk_version;
        mLibVersion = li.library_version;
    }
    mFeatureDir = dirOf(mLibPath);
    mFeaturePaths = {mFeatureDir};
    if (!cfg.viewerLibDir.empty() && cfg.viewerLibDir != mFeatureDir) mFeaturePaths.push_back(cfg.viewerLibDir);
    mFeaturePathPtrs.clear();
    for (const std::string& s : mFeaturePaths) mFeaturePathPtrs.push_back(s.c_str());

    slip_nr_app_info app;
    fillAppInfo(&app);
    uint32_t n = 0;
    const char* const* names = nullptr;
    const slip_nr_result r = mFn->instanceExtensions(&app, &n, &names);
    if (r != SLIP_NR_OK)
    {
        mReason = std::string("the shim's instance extension query failed: ") + mFn->lastError();
        // (kept loaded: nothing else will use it; shutdown() unloads)
        finishStatus();
        return false;
    }
    for (uint32_t i = 0; i < n && names; ++i)
        if (names[i]) mInstExts.emplace_back(names[i]);
    LL_INFOS("Vulkan") << "DLSS: loaded " << mLibPath << " (" << mBackend << ", SDK " << mSdkVersion << ", shim "
                       << mLibVersion << (mFn->optimalRenderSize2 ? "" : "; predates Super Resolution")
                       << "); instance extensions: " << joinNames(mInstExts) << LL_ENDL;
    return true;
}

void LLVKNeuralRecon::instanceExtensionsMissing(const std::string& missing)
{
    fail("the Vulkan loader does not offer the instance extension(s) " + missing);
}

bool LLVKNeuralRecon::deviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<std::string>& exts,
                                       std::string& why)
{
    exts.clear();
    if (!mFn || !mReason.empty())
    {
        why = mReason.empty() ? std::string("the shim is not loaded") : mReason;
        return false;
    }
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    if (p2.properties.vendorID != 0x10DE)
    {
        why = "not an NVIDIA GPU";
        fail(why);
        return false;
    }
    if (drv.driverID != VK_DRIVER_ID_NVIDIA_PROPRIETARY)
    {
        why = "not NVIDIA's own driver";
        fail(why);
        return false;
    }
    slip_nr_app_info app;
    fillAppInfo(&app);
    uint32_t n = 0;
    const char* const* names = nullptr;
    const slip_nr_result r = mFn->deviceExtensions(&app, inst, pd, &n, &names);
    if (r != SLIP_NR_OK)
    {
        why = std::string(r == SLIP_NR_ERROR_DRIVER_TOO_OLD ? "the NVIDIA driver is too old: " : "")
            + mFn->lastError();
        fail(why);
        return false;
    }
    for (uint32_t i = 0; i < n && names; ++i)
        if (names[i]) exts.emplace_back(names[i]);
    mDevExts = exts;
    return true;
}

bool LLVKNeuralRecon::initDevice(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, bool lateExtsOn,
                                 const std::string& lateWhy)
{
    mDevExtsOn = lateExtsOn;
    if (!mFn || !mReason.empty())
    {
        finishStatus();
        return false;
    }
    if (!lateExtsOn)
    {
        fail("the device extensions could not be enabled (" + lateWhy + ")");
        finishStatus();
        return false;
    }
    slip_nr_app_info app;
    fillAppInfo(&app);
    slip_nr_vulkan_device vd{};
    vd.struct_size = sizeof(vd);
    vd.instance = inst;
    vd.physical_device = pd;
    vd.device = dev;
    vd.get_instance_proc_addr = vkGetInstanceProcAddr;
    vd.get_device_proc_addr = vkGetDeviceProcAddr;
    slip_nr_result r = mFn->init(&app, &vd);
    if (r != SLIP_NR_OK)
    {
        fail(std::string("NGX initialisation failed: ") + mFn->lastError());
        finishStatus();
        return false;
    }
    mInit = true;
    mDevice = dev;
    // (zeroed: a shim before 1.2 writes only the first release's fields, so super_resolution
    // stays 0 -- and it has no SR entry points anyway)
    slip_nr_caps caps;
    std::memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    r = mFn->getCaps(&caps);
    if (r != SLIP_NR_OK)
    {
        fail(caps.reason[0] ? std::string(caps.reason) : std::string("the shim's capability query failed: ")
                                                             + mFn->lastError());
        finishStatus();
        return false;
    }
    mFeatAvail[F_RR] = caps.ray_reconstruction != 0;
    mFeatReason[F_RR] = mFeatAvail[F_RR] ? std::string()
                      : (caps.reason[0] ? std::string(caps.reason) : std::string("Ray Reconstruction is not available"));
    if (!mFn->optimalRenderSize2)
    {
        mFeatAvail[F_SR] = false;
        mFeatReason[F_SR] = "the installed shim (" + (mLibVersion.empty() ? std::string("version unknown") : mLibVersion)
                          + ") predates Super Resolution: rebuild it from this viewer's contrib/ngxshim";
    }
    else
    {
        mFeatAvail[F_SR] = caps.super_resolution != 0;
        mFeatReason[F_SR] = mFeatAvail[F_SR] ? std::string()
                          : (caps.sr_reason[0] ? std::string(caps.sr_reason) : std::string("Super Resolution is not available"));
    }
    if (!mFeatAvail[F_RR] && !mFeatAvail[F_SR])
    {
        fail("Super Resolution: " + mFeatReason[F_SR] + "; Ray Reconstruction: " + mFeatReason[F_RR]);
        finishStatus();
        return false;
    }
    mAvailable = true;
    finishStatus();
    return true;
}

void LLVKNeuralRecon::finishStatus()
{
    if (mAvailable)
        mStatus = std::string("available -- Super Resolution: ")
                + (mFeatAvail[F_SR] ? std::string("yes") : "no (" + mFeatReason[F_SR] + ")")
                + "; Ray Reconstruction: " + (mFeatAvail[F_RR] ? std::string("yes") : "no (" + mFeatReason[F_RR] + ")")
                + " (" + mLibPath + ", " + mBackend + " SDK " + mSdkVersion + ", shim " + mLibVersion
                + "; device extensions: " + joinNames(mDevExts)
                + (supportsOverlay() ? "; transparency overlay + responsivity" : "; no transparency overlay: rebuild the shim from this viewer's contrib/ngxshim")
                + ")";
    else
        mStatus = "unavailable: " + (mReason.empty() ? std::string("not initialised") : mReason);
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        LL_INFOS("Vulkan") << "DLSS: " << mStatus << LL_ENDL;
    }
}

void LLVKNeuralRecon::shutdown()
{
    if (mFn && mInit) mFn->shutdown();
    mInit = false;
    mAvailable = false;
    mFeatAvail[0] = mFeatAvail[1] = false;
    mDevice = VK_NULL_HANDLE;
    unload();
    // A later LLVulkanContext::init (a re-created device) loads it again from scratch.
    mTried = false;
    mReason = "shut down";
    mInstExts.clear();
    mDevExts.clear();
    mDevExtsOn = false;
}

bool LLVKNeuralRecon::supportsOverlay() const
{
    unsigned maj = 0, min = 0;
    if (std::sscanf(mLibVersion.c_str(), "%u.%u", &maj, &min) != 2) return false;
    return maj > 1 || (maj == 1 && min >= 1);
}

bool LLVKNeuralRecon::optimalRenderSize(FeatureKind k, Quality q, uint32_t dw, uint32_t dh, uint32_t& rw, uint32_t& rh)
{
    rw = rh = 0;
    if (!available(k) || !mFn || !dw || !dh) return false;
    if (q == Q_DLAA) { rw = dw; rh = dh; return true; }
    slip_nr_result r = SLIP_NR_ERROR_UNSUPPORTED;
    if (mFn->optimalRenderSize2)
        r = mFn->optimalRenderSize2((uint32_t)k, (slip_nr_quality)q, dw, dh, &rw, &rh, nullptr, nullptr, nullptr, nullptr);
    else if (k == F_RR)
        r = mFn->optimalRenderSize((slip_nr_quality)q, dw, dh, &rw, &rh, nullptr, nullptr, nullptr, nullptr);
    return r == SLIP_NR_OK && rw && rh;
}

LLVKNeuralRecon::Feature* LLVKNeuralRecon::create(VkCommandBuffer cmd, FeatureKind k, uint32_t rw, uint32_t rh,
                                                  uint32_t dw, uint32_t dh, Quality q, uint32_t flags, std::string& err)
{
    if (!available(k) || !mFn) { err = reason(k); return nullptr; }
    slip_nr_create_info ci{};
    // (an older shim reads only the first release's fields: it can only be asked for RR, as
    // available(F_SR) is false for it)
    ci.struct_size = sizeof(ci);
    ci.feature = (uint32_t)k;
    ci.render_width = rw; ci.render_height = rh;
    ci.display_width = dw; ci.display_height = dh;
    ci.quality = (slip_nr_quality)q;
    ci.flags = flags;
    ci.preset = 0;
    slip_nr_feature* f = nullptr;
    if (mFn->create(cmd, &ci, &f) != SLIP_NR_OK || !f)
    {
        err = mFn->lastError();
        return nullptr;
    }
    Feature* out = new Feature();
    out->f = f;
    return out;
}

void LLVKNeuralRecon::destroy(Feature* f)
{
    if (!f) return;
    if (mFn && f->f) mFn->destroy(f->f);
    delete f;
}

bool LLVKNeuralRecon::evaluate(VkCommandBuffer cmd, Feature* f, const Eval& e, std::string& err)
{
    if (!mAvailable || !mFn || !f || !f->f) { err = "no feature"; return false; }
    slip_nr_eval_info ei{};
    ei.struct_size = sizeof(ei);
    ei.color = toImage(e.color);
    ei.output = toImage(e.output);
    ei.depth = toImage(e.depth);
    ei.motion = toImage(e.motion);
    ei.diffuse_albedo = toImage(e.diffuseAlbedo);
    ei.specular_albedo = toImage(e.specularAlbedo);
    ei.normals = toImage(e.normals);
    ei.specular_hit_distance = toImage(e.specHitDist);
    ei.exposure = toImage(e.exposure);
    ei.transparency_layer = toImage(e.transparency);
    ei.responsivity = toImage(e.responsivity);
    ei.jitter_x = e.jitter[0];
    ei.jitter_y = e.jitter[1];
    ei.mv_scale_x = e.mvScale[0];
    ei.mv_scale_y = e.mvScale[1];
    ei.render_width = e.rw;
    ei.render_height = e.rh;
    ei.reset = e.reset ? 1u : 0u;
    ei.frame_time_ms = e.frameTimeMs;
    ei.pre_exposure = e.preExposure > 0.f ? e.preExposure : 1.f;
    std::memcpy(ei.world_to_view, e.worldToView, sizeof(ei.world_to_view));
    std::memcpy(ei.view_to_clip, e.viewToClip, sizeof(ei.view_to_clip));
    if (mFn->evaluate(cmd, f->f, &ei) != SLIP_NR_OK)
    {
        err = mFn->lastError();
        return false;
    }
    return true;
}
