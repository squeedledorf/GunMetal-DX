/**
 * @file llvkneuralrecon.h
 * @brief The optional, run-time-loaded neural denoise + upscale library (#870, #962): today
 *        the user-built DLSS shim (contrib/ngxshim, MIT) -- Ray Reconstruction and, from shim
 *        1.2, Super Resolution -- reached only through its plain-C "slip_nr" ABI
 *        (contrib/ngxshim/include/slipstream_ngx.h).
 *
 * The viewer contains no vendor code: this class dlopen()s / LoadLibrary()s
 * libslipstream_ngx.so / slipstream_ngx.dll from the configured folder (RenderVKDlssShimPath)
 * or the viewer's own library folder, resolves the slip_nr_* entry points and drives them:
 *
 *   load()                         before vkCreateInstance (LLVulkanContext::init)
 *   instanceExtensions()           -> appended to the instance (those the loader lists)
 *   deviceExtensionProvider()      -> VkDeviceContext::setLateExtensionProvider (NVIDIA only)
 *   initDevice()                   after vkCreateDevice: NGX init + capability query
 *   create / evaluate / destroy    LLCleanDlssRRVK (render thread)
 *   shutdown()                     before vkDestroyDevice
 *
 * A missing or failing library is never fatal: available() stays false and reason() says
 * why (the preferences tooltip and the one "DLSS:" status line in the log read it). Each
 * FEATURE (F_RR, F_SR) has its own availability + reason: each needs its own NVIDIA library,
 * and a shim older than 1.2 has no SR at all (detected by its missing
 * slip_nr_optimal_render_size2 entry point).
 *
 * Licensing note (option B): nothing NVIDIA ships with the viewer, so there is no NVIDIA
 * attribution today. attributionNotice() is the ONE place a future build that distributes
 * the shim + NVIDIA's feature library ("option A") would return the required notice for the
 * About box / credits; licenses-*.txt would then gain the SDK's third-party notices.
 */
#ifndef LL_LLVKNEURALRECON_H
#define LL_LLVKNEURALRECON_H

#include "vk_common.h"

#include <cstdint>
#include <string>
#include <vector>

class LLVKNeuralRecon
{
public:
    static LLVKNeuralRecon& instance();

    // Fixed quality modes (mirror slip_nr_quality).
    enum Quality : uint32_t { Q_DLAA = 0, Q_QUALITY = 1, Q_BALANCED = 2, Q_PERFORMANCE = 3, Q_ULTRA_PERFORMANCE = 4 };
    // The network (mirror slip_nr_feature_type).
    enum FeatureKind : uint32_t { F_RR = 0, F_SR = 1 };
    static const char* kindName(FeatureKind k) { return k == F_SR ? "SR" : "RR"; }
    static const char* qualityName(Quality q);

    struct Config
    {
        bool        enabled = true;        // RenderVKDlssShim (0 = never load the library)
        std::string configuredPath;        // RenderVKDlssShimPath: a folder or the library file
        std::string viewerLibDir;          // the viewer's own library folder
        std::string defaultDir;            // <user data dir>/dlss, looked in when configuredPath is empty (contrib/ngxshim/build.sh)
        std::string dataDir;               // writable folder for the vendor runtime (logs / caches)
        std::string engineVersion;         // the viewer version string
    };

    // Before vkCreateInstance. Idempotent; false = not loaded (reason() says why).
    bool load(const Config& cfg);
    bool loaded() const { return mLib != nullptr; }
    // The instance extensions the library needs (empty when not loaded / it failed).
    const std::vector<std::string>& instanceExtensions() const { return mInstExts; }
    // Record that the instance could not enable all of them (some not listed by the loader).
    void instanceExtensionsMissing(const std::string& missing);
    // For VkDeviceContext::setLateExtensionProvider: NVIDIA proprietary driver + the library's
    // device extensions; false (+ why) otherwise.
    bool deviceExtensions(VkInstance inst, VkPhysicalDevice pd, std::vector<std::string>& exts, std::string& why);
    // After vkCreateDevice. lateExtsOn: VkDeviceContext::lateExtensionsEnabled(); lateWhy its reason.
    bool initDevice(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, bool lateExtsOn, const std::string& lateWhy);
    void shutdown();   // before vkDestroyDevice (the GPU idle)

    // At least one feature is usable this session (library loaded, extensions enabled, NGX
    // initialised, the feature reported available).
    bool available() const { return mAvailable; }
    // Why not (empty when available).
    const std::string& reason() const { return mReason; }
    // This feature is usable (its NVIDIA library found, the GPU / driver run it, the shim has it).
    bool available(FeatureKind k) const { return mAvailable && mFeatAvail[k & 1u]; }
    // Why not (empty when available).
    const std::string& reason(FeatureKind k) const { return mAvailable ? mFeatReason[k & 1u] : mReason; }
    // The status line (also logged once).
    const std::string& status() const { return mStatus; }

    // The loaded shim reads slip_nr_eval_info.transparency_layer / responsivity (library
    // version >= 1.1; an older build ignores them, so the viewer must not rely on them).
    bool supportsOverlay() const;
    // The render extent the feature wants for this display extent + mode (false: unavailable).
    bool optimalRenderSize(FeatureKind k, Quality q, uint32_t dw, uint32_t dh, uint32_t& rw, uint32_t& rh);

    // Opaque feature (slip_nr_feature*).
    struct Feature;
    // Records into cmd (a command buffer that is NOT a frame's: see LLCleanDlssRRVK).
    Feature* create(VkCommandBuffer cmd, FeatureKind k, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh,
                    Quality q, uint32_t flags, std::string& err);
    void     destroy(Feature* f);   // the GPU no longer uses it

    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t w = 0, h = 0;
        VkImageAspectFlags aspect = 0;
    };
    struct Eval
    {
        Image color, output, depth, motion, diffuseAlbedo, specularAlbedo, normals, specHitDist, exposure;
        Image transparency;   // premultiplied RGBA overlay (upscaled, not denoised); color is then the opaque scene
        Image responsivity;   // R16F [-1, 1] (SR: the bias-current-colour mask, [0, 1])
        float jitter[2] = {0.f, 0.f};
        float mvScale[2] = {1.f, 1.f};
        uint32_t rw = 0, rh = 0;
        bool  reset = false;
        float frameTimeMs = 16.6f;
        float preExposure = 1.f;
        float worldToView[16] = {};   // ROW-major
        float viewToClip[16] = {};    // ROW-major
    };
    bool evaluate(VkCommandBuffer cmd, Feature* f, const Eval& e, std::string& err);

    // Create flags (mirror SLIP_NR_CREATE_*).
    static constexpr uint32_t kHdr = 0x1u, kDepthInverted = 0x2u, kMvRenderRes = 0x4u, kMvJittered = 0x8u,
                              kRoughnessPacked = 0x10u, kHwDepth = 0x20u, kAutoExposure = 0x40u;

    // The single place for a future NVIDIA attribution (option A). nullptr today (option B:
    // nothing NVIDIA is distributed with the viewer).
    static const char* attributionNotice() { return nullptr; }

    // Forwarded vendor log lines (thread safe; LL_DEBUGS("NGX")).
    static void logLine(uint32_t level, const char* msg);

private:
    LLVKNeuralRecon() = default;
    void* sym(const char* name);
    void  fail(const std::string& why);
    void  unload();
    void  finishStatus();
    void  fillAppInfo(void* app);   // slip_nr_app_info*

    void*       mLib = nullptr;
    std::string mLibPath;
    bool        mTried = false;
    bool        mAvailable = false;
    bool        mFeatAvail[2] = {false, false};   // per FeatureKind
    std::string mFeatReason[2];
    std::string mReason, mStatus;
    std::string mBackend, mSdkVersion, mLibVersion;
    std::vector<std::string> mInstExts;
    std::vector<std::string> mDevExts;
    bool        mDevExtsOn = false;
    Config      mCfg;
    std::string mFeatureDir;            // the library's folder (the vendor feature library lives there)
    std::vector<std::string> mFeaturePaths;
    std::vector<const char*> mFeaturePathPtrs;
    bool        mInit = false;
    VkDevice    mDevice = VK_NULL_HANDLE;
    struct Fns;
    Fns*        mFn = nullptr;
};

#endif
