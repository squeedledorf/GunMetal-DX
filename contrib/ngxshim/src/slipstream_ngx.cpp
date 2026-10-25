/*
 * slipstream_ngx.cpp -- the slip_nr C ABI (include/slipstream_ngx.h) implemented on NVIDIA NGX
 * (DLSS Ray Reconstruction and DLSS Super Resolution, Vulkan).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See ../LICENSE.
 *
 * This file is original code written for this shim. It includes the NVIDIA DLSS SDK headers
 * and links the SDK's NGX loader (libnvsdk_ngx.a / nvsdk_ngx_*.lib) from the folder given
 * as DLSS_SDK_DIR; neither is part of this project (see README.md).
 */
#define SLIP_NR_BUILDING_LIBRARY 1
#include "slipstream_ngx.h"

#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"
#include "nvsdk_ngx_helpers_dlssd_vk.h"
#include "nvsdk_ngx_defs_dlssd.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifndef SLIP_NR_SDK_VERSION
#  define SLIP_NR_SDK_VERSION "unknown"
#endif
// 1.1.0: slip_nr_eval_info.transparency_layer / responsivity (the viewer only fills them for >= 1.1)
// 1.2.0: DLSS Super Resolution next to RR (slip_nr_create_info.feature, slip_nr_caps.super_resolution..,
//        slip_nr_optimal_render_size2); either feature may be missing (its own NVIDIA library)
#define SLIP_NR_LIBRARY_VERSION "1.2.0"

// Used when the application passes no project id (NGX wants a GUID-style string; this one
// identifies "a Slipstream build of this shim" with NVSDK_NGX_ENGINE_TYPE_CUSTOM).
static const char* kDefaultProjectId = "5d1f8a30-870c-4c0e-9b8e-51195e7a0870";

struct slip_nr_feature
{
    uint32_t             type = SLIP_NR_FEATURE_RAY_RECONSTRUCTION;   // slip_nr_feature_type
    NVSDK_NGX_Handle*    handle = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    bool                 ownParams = false;
    uint32_t             flags = 0;
};

namespace
{
std::mutex       g_logMutex;
slip_nr_log_fn   g_logFn = nullptr;
void*            g_logUser = nullptr;
std::string      g_lastError;

bool                 g_init = false;
VkDevice             g_device = VK_NULL_HANDLE;
NVSDK_NGX_Parameter* g_caps = nullptr;

// Extension name storage (returned to the caller; valid until shutdown / unload).
std::vector<std::string> g_instNames, g_devNames;
std::vector<const char*> g_instPtrs, g_devPtrs;

// Path storage for NVSDK_NGX_FeatureCommonInfo (NGX may keep the pointers).
std::wstring              g_dataPath;
std::vector<std::wstring> g_paths;
std::vector<const wchar_t*> g_pathPtrs;
std::string               g_projectId, g_engineVersion;
NVSDK_NGX_FeatureCommonInfo g_common{};
// Why a feature's requirement query refused it (device_extensions; the caps reason uses it).
std::string               g_reqWhy[2];

void setError(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_lastError = buf;
}

void log(uint32_t level, const char* msg)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFn) g_logFn(g_logUser, level, msg);
}

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature)
{
    log(level >= NVSDK_NGX_LOGGING_LEVEL_VERBOSE ? SLIP_NR_LOG_VERBOSE : SLIP_NR_LOG_INFO, message);
}

// UTF-8 -> wchar_t (UTF-32 where wchar_t is 4 bytes, UTF-16 on Windows).
std::wstring widen(const char* s)
{
    std::wstring out;
    if (!s) return out;
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n > 1)
    {
        out.resize((size_t)n - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    }
#else
    const unsigned char* p = (const unsigned char*)s;
    while (*p)
    {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0xF0) { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        for (; extra > 0 && (*p & 0xC0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3F);
        out.push_back((wchar_t)c);
    }
#endif
    return out;
}

std::string narrow(const wchar_t* w)
{
    std::string out;
    if (!w) return out;
    for (; *w; ++w)
    {
        const uint32_t c = (uint32_t)*w;
        if (c < 0x80) out.push_back((char)c);
        else out.push_back('?');   // (diagnostic text only)
    }
    return out;
}

std::string resultText(NVSDK_NGX_Result r)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "0x%08x", (unsigned)r);
    std::string s = narrow(GetNGXResultAsString(r));
    return s.empty() ? std::string(buf) : s + " (" + buf + ")";
}

slip_nr_result mapResult(NVSDK_NGX_Result r)
{
    if (!NVSDK_NGX_FAILED(r)) return SLIP_NR_OK;
    switch (r)
    {
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
    case NVSDK_NGX_Result_FAIL_PlatformError:      return SLIP_NR_ERROR_UNSUPPORTED;
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:    return SLIP_NR_ERROR_FEATURE_MISSING;
    case NVSDK_NGX_Result_FAIL_InvalidParameter:
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:
    case NVSDK_NGX_Result_FAIL_MissingInput:
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return SLIP_NR_ERROR_INVALID_ARGUMENT;
    case NVSDK_NGX_Result_FAIL_NotInitialized:     return SLIP_NR_ERROR_NOT_INITIALIZED;
    case NVSDK_NGX_Result_FAIL_OutOfDate:          return SLIP_NR_ERROR_DRIVER_TOO_OLD;
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:     return SLIP_NR_ERROR_OUT_OF_MEMORY;
    default:                                       return SLIP_NR_ERROR_FAILED;
    }
}

slip_nr_result fail(const char* what, NVSDK_NGX_Result r)
{
    setError("%s failed: %s", what, resultText(r).c_str());
    return mapResult(r);
}

// The app info -> NGX's common info + discovery info (stored in globals: NGX may keep them).
void applyAppInfo(const slip_nr_app_info* app)
{
    g_projectId = (app && app->project_id && *app->project_id) ? app->project_id : kDefaultProjectId;
    g_engineVersion = (app && app->engine_version) ? app->engine_version : "0";
    g_dataPath = widen(app ? app->app_data_path : nullptr);
    g_paths.clear();
    g_pathPtrs.clear();
    if (app && app->feature_paths)
        for (uint32_t i = 0; i < app->feature_path_count; ++i)
            if (app->feature_paths[i] && *app->feature_paths[i]) g_paths.push_back(widen(app->feature_paths[i]));
    for (const std::wstring& p : g_paths) g_pathPtrs.push_back(p.c_str());
    g_common = NVSDK_NGX_FeatureCommonInfo{};
    g_common.PathListInfo.Path = g_pathPtrs.empty() ? nullptr : g_pathPtrs.data();
    g_common.PathListInfo.Length = (unsigned int)g_pathPtrs.size();
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logFn = app ? app->log_fn : nullptr;
        g_logUser = app ? app->log_user : nullptr;
    }
    if (app && app->log_fn)
    {
        g_common.LoggingInfo.LoggingCallback = ngxLog;
        g_common.LoggingInfo.MinimumLoggingLevel = app->log_verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE
                                                                    : NVSDK_NGX_LOGGING_LEVEL_ON;
        // The application's log only (NGX would also print every line to stdout).
        g_common.LoggingInfo.DisableOtherLoggingSinks = true;
    }
}

const char* featureName(uint32_t type)
{
    return type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? "Super Resolution" : "Ray Reconstruction";
}

NVSDK_NGX_Feature ngxFeature(uint32_t type)
{
    return type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? NVSDK_NGX_Feature_SuperSampling
                                                    : NVSDK_NGX_Feature_RayReconstruction;
}

NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo(uint32_t type)
{
    NVSDK_NGX_FeatureDiscoveryInfo d{};
    d.SDKVersion = NVSDK_NGX_Version_API;
    d.FeatureID = ngxFeature(type);
    d.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    d.Identifier.v.ProjectDesc.ProjectId = g_projectId.c_str();
    d.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    d.Identifier.v.ProjectDesc.EngineVersion = g_engineVersion.c_str();
    d.ApplicationDataPath = g_dataPath.empty() ? L"." : g_dataPath.c_str();
    d.FeatureInfo = &g_common;
    return d;
}

// Appends (deduplicated); finishNames() then points ptrs at the strings.
void addNames(uint32_t n, const VkExtensionProperties* props, std::vector<std::string>& names)
{
    for (uint32_t i = 0; i < n && props; ++i)
    {
        const std::string e(props[i].extensionName);
        bool have = false;
        for (const std::string& x : names) have = have || x == e;
        if (!have) names.push_back(e);
    }
}

void finishNames(const std::vector<std::string>& names, std::vector<const char*>& ptrs)
{
    ptrs.clear();
    for (const std::string& s : names) ptrs.push_back(s.c_str());
}

void copyStr(char* dst, size_t cap, const char* src)
{
    std::snprintf(dst, cap, "%s", src ? src : "");
}

NVSDK_NGX_PerfQuality_Value perfQuality(slip_nr_quality q)
{
    switch (q)
    {
    case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_PerfQuality_Value_DLAA;
    case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_PerfQuality_Value_Balanced;
    case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    default:                                return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
}

const char* presetParam(uint32_t type, slip_nr_quality q)
{
    if (type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        switch (q)
        {
        case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
        case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
        case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
        default:                                return nullptr;
        }
    }
    switch (q)
    {
    case SLIP_NR_QUALITY_DLAA:              return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA;
    case SLIP_NR_QUALITY_QUALITY:           return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality;
    case SLIP_NR_QUALITY_BALANCED:          return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced;
    case SLIP_NR_QUALITY_PERFORMANCE:       return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance;
    case SLIP_NR_QUALITY_ULTRA_PERFORMANCE: return NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance;
    default:                                return nullptr;
    }
}

// slip_nr_image -> NGX's Vulkan resource wrapper (nullptr when not provided).
NVSDK_NGX_Resource_VK* wrap(const slip_nr_image& im, NVSDK_NGX_Resource_VK& storage, bool writable)
{
    if (!im.image || !im.view) return nullptr;
    VkImageSubresourceRange r{};
    r.aspectMask = im.aspect ? im.aspect : VK_IMAGE_ASPECT_COLOR_BIT;
    r.baseMipLevel = 0; r.levelCount = 1;
    r.baseArrayLayer = 0; r.layerCount = 1;
    storage = NVSDK_NGX_Create_ImageView_Resource_VK(im.view, im.image, r, im.format, im.width, im.height, writable);
    return &storage;
}
} // namespace

extern "C" {

SLIP_NR_API uint32_t SLIP_NR_CALL slip_nr_abi_version(void)
{
    return SLIP_NR_ABI_VERSION;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_info(slip_nr_library_info* out)
{
    if (!out || out->struct_size < sizeof(slip_nr_library_info)) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    out->abi_version = SLIP_NR_ABI_VERSION;
    copyStr(out->backend, sizeof(out->backend), "NVIDIA NGX");
    copyStr(out->sdk_version, sizeof(out->sdk_version), SLIP_NR_SDK_VERSION);
    copyStr(out->library_version, sizeof(out->library_version), SLIP_NR_LIBRARY_VERSION);
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_instance_extensions(const slip_nr_app_info* app,
                                                                    uint32_t* count, const char* const** names)
{
    if (!count || !names) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *count = 0; *names = nullptr;
    applyAppInfo(app);
    // Both features' extensions (union): either one alone is enough to go on.
    g_instNames.clear();
    bool any = false;
    NVSDK_NGX_Result firstFail = NVSDK_NGX_Result_Success;
    for (uint32_t type : {(uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION, (uint32_t)SLIP_NR_FEATURE_SUPER_RESOLUTION})
    {
        NVSDK_NGX_FeatureDiscoveryInfo d = discoveryInfo(type);
        uint32_t n = 0;
        VkExtensionProperties* props = nullptr;
        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&d, &n, &props);
        if (NVSDK_NGX_FAILED(r))
        {
            if (firstFail == NVSDK_NGX_Result_Success) firstFail = r;
            continue;
        }
        any = true;
        addNames(n, props, g_instNames);
    }
    if (!any) return fail("NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements", firstFail);
    finishNames(g_instNames, g_instPtrs);
    *count = (uint32_t)g_instPtrs.size();
    *names = g_instPtrs.empty() ? nullptr : g_instPtrs.data();
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_device_extensions(const slip_nr_app_info* app, VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  uint32_t* count, const char* const** names)
{
    if (!count || !names || !instance || !physical_device) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *count = 0; *names = nullptr;
    applyAppInfo(app);
    // Per feature: supported on this GPU / OS / driver at all? The union of the supported ones'
    // extensions; fails only when neither is supported (the error names both reasons).
    g_devNames.clear();
    bool any = false, driverOld = false;
    std::string why;
    for (uint32_t type : {(uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION, (uint32_t)SLIP_NR_FEATURE_SUPER_RESOLUTION})
    {
        g_reqWhy[type].clear();
        NVSDK_NGX_FeatureDiscoveryInfo d = discoveryInfo(type);
        NVSDK_NGX_FeatureRequirement req{};
        NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical_device, &d, &req);
        char buf[256];
        if (NVSDK_NGX_FAILED(r))
        {
            std::snprintf(buf, sizeof(buf), "%s: the requirement query failed: %s", featureName(type),
                          resultText(r).c_str());
            g_reqWhy[type] = buf;
        }
        else if (req.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported)
        {
            std::snprintf(buf, sizeof(buf), "%s is not supported here (support flags 0x%x, minimum GPU architecture "
                          "0x%x, minimum OS %s)", featureName(type), (unsigned)req.FeatureSupported,
                          req.MinHWArchitecture, req.MinOSVersion);
            g_reqWhy[type] = buf;
            if (req.FeatureSupported & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) driverOld = true;
        }
        else
        {
            uint32_t n = 0;
            VkExtensionProperties* props = nullptr;
            r = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical_device, &d, &n, &props);
            if (NVSDK_NGX_FAILED(r))
            {
                std::snprintf(buf, sizeof(buf), "%s: the device extension query failed: %s", featureName(type),
                              resultText(r).c_str());
                g_reqWhy[type] = buf;
            }
            else
            {
                any = true;
                addNames(n, props, g_devNames);
            }
        }
        if (!g_reqWhy[type].empty()) why += (why.empty() ? "" : "; ") + g_reqWhy[type];
    }
    if (!any)
    {
        setError("%s", why.c_str());
        return driverOld ? SLIP_NR_ERROR_DRIVER_TOO_OLD : SLIP_NR_ERROR_UNSUPPORTED;
    }
    finishNames(g_devNames, g_devPtrs);
    *count = (uint32_t)g_devPtrs.size();
    *names = g_devPtrs.empty() ? nullptr : g_devPtrs.data();
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_init(const slip_nr_app_info* app, const slip_nr_vulkan_device* dev)
{
    if (g_init) return SLIP_NR_OK;
    if (!app || !dev || dev->struct_size < sizeof(slip_nr_vulkan_device) || !dev->instance || !dev->physical_device
        || !dev->device || !app->app_data_path || !*app->app_data_path)
    {
        setError("slip_nr_init: missing instance / device / app_data_path");
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    applyAppInfo(app);
    // Over-the-air model updates are never requested (NVSDK_NGX_UpdateFeature is not called).
    const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        g_projectId.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, g_engineVersion.c_str(), g_dataPath.c_str(),
        dev->instance, dev->physical_device, dev->device, dev->get_instance_proc_addr, dev->get_device_proc_addr,
        &g_common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) return fail("NVSDK_NGX_VULKAN_Init_with_ProjectID", r);
    g_init = true;
    g_device = dev->device;
    const NVSDK_NGX_Result c = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_FAILED(c))
    {
        g_caps = nullptr;
        return fail("NVSDK_NGX_VULKAN_GetCapabilityParameters", c);
    }
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_caps(slip_nr_caps* out)
{
    // (An ABI-1 caller from before 1.2 passes the shorter struct: only what fits is written.)
    if (!out || out->struct_size < SLIP_NR_CAPS_SIZE_V1) return SLIP_NR_ERROR_INVALID_ARGUMENT;
    const uint32_t size = out->struct_size;
    const bool v12 = size >= sizeof(slip_nr_caps);
    std::memset(out, 0, v12 ? sizeof(slip_nr_caps) : SLIP_NR_CAPS_SIZE_V1);
    out->struct_size = size;
    if (!g_init || !g_caps)
    {
        copyStr(out->reason, sizeof(out->reason), "NGX is not initialised");
        if (v12) copyStr(out->sr_reason, sizeof(out->sr_reason), "NGX is not initialised");
        return SLIP_NR_ERROR_NOT_INITIALIZED;
    }
    struct One { uint32_t avail, needs, maj, minr, init; };
    auto query = [](const char* pAvail, const char* pNeeds, const char* pMaj, const char* pMin, const char* pInit)
    {
        int avail = 0, needs = 0, initRes = 0;
        unsigned int maj = 0, minr = 0;
        NVSDK_NGX_Parameter_GetI(g_caps, pAvail, &avail);
        NVSDK_NGX_Parameter_GetI(g_caps, pNeeds, &needs);
        NVSDK_NGX_Parameter_GetUI(g_caps, pMaj, &maj);
        NVSDK_NGX_Parameter_GetUI(g_caps, pMin, &minr);
        NVSDK_NGX_Parameter_GetI(g_caps, pInit, &initRes);
        return One{avail ? 1u : 0u, needs ? 1u : 0u, maj, minr, (uint32_t)initRes};
    };
    auto reason = [](uint32_t type, const One& o, char* dst, size_t cap)
    {
        if (o.avail) return;
        if (o.needs)
            std::snprintf(dst, cap, "the NVIDIA driver is too old for %s (needs %u.%u or newer)", featureName(type),
                          o.maj, o.minr);
        else if (!g_reqWhy[type].empty())
            std::snprintf(dst, cap, "%s", g_reqWhy[type].c_str());
        else
            std::snprintf(dst, cap, "%s is not available (feature init %s; is its NVIDIA library, %s, installed?)",
                          featureName(type), resultText((NVSDK_NGX_Result)o.init).c_str(),
                          type == SLIP_NR_FEATURE_SUPER_RESOLUTION ? "nvngx_dlss" : "nvngx_dlssd");
    };
    const One rr = query(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMajor,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_MinDriverVersionMinor,
                         NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult);
    out->ray_reconstruction = rr.avail;
    out->needs_newer_driver = rr.needs;
    out->min_driver_major = rr.maj;
    out->min_driver_minor = rr.minr;
    out->vendor_init_result = rr.init;
    reason(SLIP_NR_FEATURE_RAY_RECONSTRUCTION, rr, out->reason, sizeof(out->reason));
    if (v12)
    {
        const One sr = query(NVSDK_NGX_Parameter_SuperSampling_Available,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                             NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor,
                             NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor,
                             NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult);
        out->super_resolution = sr.avail;
        out->sr_needs_newer_driver = sr.needs;
        out->sr_min_driver_major = sr.maj;
        out->sr_min_driver_minor = sr.minr;
        out->sr_vendor_init_result = sr.init;
        reason(SLIP_NR_FEATURE_SUPER_RESOLUTION, sr, out->sr_reason, sizeof(out->sr_reason));
    }
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size(slip_nr_quality quality,
                                                                    uint32_t display_width, uint32_t display_height,
                                                                    uint32_t* render_width, uint32_t* render_height,
                                                                    uint32_t* min_width, uint32_t* min_height,
                                                                    uint32_t* max_width, uint32_t* max_height)
{
    return slip_nr_optimal_render_size2(SLIP_NR_FEATURE_RAY_RECONSTRUCTION, quality, display_width, display_height,
                                        render_width, render_height, min_width, min_height, max_width, max_height);
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size2(uint32_t feature, slip_nr_quality quality,
                                                                     uint32_t display_width, uint32_t display_height,
                                                                     uint32_t* render_width, uint32_t* render_height,
                                                                     uint32_t* min_width, uint32_t* min_height,
                                                                     uint32_t* max_width, uint32_t* max_height)
{
    if (!render_width || !render_height || !display_width || !display_height
        || (feature != SLIP_NR_FEATURE_RAY_RECONSTRUCTION && feature != SLIP_NR_FEATURE_SUPER_RESOLUTION))
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    if (!g_init || !g_caps) return SLIP_NR_ERROR_NOT_INITIALIZED;
    void* cb = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, feature == SLIP_NR_FEATURE_SUPER_RESOLUTION
                                                   ? NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback
                                                   : NVSDK_NGX_Parameter_DLSSDOptimalSettingsCallback, &cb);
    if (!cb)
    {
        setError("the installed %s library has no optimal-settings callback (missing or out of date?)",
                 featureName(feature));
        return SLIP_NR_ERROR_DRIVER_TOO_OLD;
    }
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, display_width);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, display_height);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, perfQuality(quality));
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);
    const NVSDK_NGX_Result r = ((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)cb)(g_caps);
    if (NVSDK_NGX_FAILED(r)) return fail("the optimal-settings callback", r);
    unsigned int w = 0, h = 0, mnw = 0, mnh = 0, mxw = 0, mxh = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, &w);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, &h);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &mnw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &mnh);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &mxw);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &mxh);
    if (!w || !h)
    {
        setError("the optimal-settings callback returned no render size for mode %u", (unsigned)quality);
        return SLIP_NR_ERROR_UNSUPPORTED;
    }
    *render_width = w; *render_height = h;
    if (min_width) *min_width = mnw ? mnw : w;
    if (min_height) *min_height = mnh ? mnh : h;
    if (max_width) *max_width = mxw ? mxw : w;
    if (max_height) *max_height = mxh ? mxh : h;
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_create(VkCommandBuffer cmd, const slip_nr_create_info* info,
                                                       slip_nr_feature** out)
{
    if (!cmd || !info || !out || info->struct_size < SLIP_NR_CREATE_INFO_SIZE_V1 || !info->render_width
        || !info->render_height || !info->display_width || !info->display_height)
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!g_init) return SLIP_NR_ERROR_NOT_INITIALIZED;
    // (a caller from before 1.2 has no feature field: Ray Reconstruction, the only feature then)
    const uint32_t type = info->struct_size >= sizeof(slip_nr_create_info) ? info->feature
                                                                           : (uint32_t)SLIP_NR_FEATURE_RAY_RECONSTRUCTION;
    if (type != SLIP_NR_FEATURE_RAY_RECONSTRUCTION && type != SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        setError("slip_nr_create: unknown feature %u", type);
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }

    slip_nr_feature* f = new slip_nr_feature();
    f->type = type;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_AllocateParameters(&f->params);
    if (NVSDK_NGX_FAILED(r))
    {
        // (The SDK's GetParameters fallback for very old drivers is deprecated; such a driver
        // can't run these features anyway.)
        delete f;
        return fail("NVSDK_NGX_VULKAN_AllocateParameters", r);
    }
    f->ownParams = true;
    f->flags = info->flags;

    NVSDK_NGX_Parameter_SetUI(f->params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    if (info->preset)
    {
        const char* p = presetParam(type, info->quality);
        if (p) NVSDK_NGX_Parameter_SetUI(f->params, p, info->preset);
    }
    int ff = NVSDK_NGX_DLSS_Feature_Flags_None;
    if (info->flags & SLIP_NR_CREATE_HDR)            ff |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (info->flags & SLIP_NR_CREATE_DEPTH_INVERTED) ff |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (info->flags & SLIP_NR_CREATE_MV_RENDER_RES)  ff |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (info->flags & SLIP_NR_CREATE_MV_JITTERED)    ff |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;

    if (type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        if (info->flags & SLIP_NR_CREATE_AUTO_EXPOSURE) ff |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        NVSDK_NGX_DLSS_Create_Params sp{};
        sp.Feature.InWidth = info->render_width;
        sp.Feature.InHeight = info->render_height;
        sp.Feature.InTargetWidth = info->display_width;
        sp.Feature.InTargetHeight = info->display_height;
        sp.Feature.InPerfQualityValue = perfQuality(info->quality);
        sp.InFeatureCreateFlags = ff;
        sp.InEnableOutputSubrects = false;
        r = NGX_VULKAN_CREATE_DLSS_EXT1(g_device, cmd, 1, 1, &f->handle, f->params, &sp);
        if (NVSDK_NGX_FAILED(r) || !f->handle)
        {
            const slip_nr_result e = fail("Super Resolution feature creation", r);
            if (f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
            delete f;
            return e == SLIP_NR_OK ? SLIP_NR_ERROR_FAILED : e;
        }
        *out = f;
        return SLIP_NR_OK;
    }

    NVSDK_NGX_DLSSD_Create_Params cp{};
    cp.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
    cp.InRoughnessMode = (info->flags & SLIP_NR_CREATE_ROUGHNESS_PACKED) ? NVSDK_NGX_DLSS_Roughness_Mode_Packed
                                                                         : NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
    cp.InUseHWDepth = (info->flags & SLIP_NR_CREATE_HW_DEPTH) ? NVSDK_NGX_DLSS_Depth_Type_HW
                                                              : NVSDK_NGX_DLSS_Depth_Type_Linear;
    cp.InWidth = info->render_width;
    cp.InHeight = info->render_height;
    cp.InTargetWidth = info->display_width;
    cp.InTargetHeight = info->display_height;
    cp.InPerfQualityValue = perfQuality(info->quality);
    cp.InFeatureCreateFlags = ff;
    cp.InEnableOutputSubrects = false;
    r = NGX_VULKAN_CREATE_DLSSD_EXT1(g_device, cmd, 1, 1, &f->handle, f->params, &cp);
    if (NVSDK_NGX_FAILED(r) || !f->handle)
    {
        const slip_nr_result e = fail("Ray Reconstruction feature creation", r);
        if (f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
        delete f;
        return e == SLIP_NR_OK ? SLIP_NR_ERROR_FAILED : e;
    }
    *out = f;
    return SLIP_NR_OK;
}

SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_evaluate(VkCommandBuffer cmd, slip_nr_feature* f,
                                                         const slip_nr_eval_info* info)
{
    if (!cmd || !f || !f->handle || !info || info->struct_size < SLIP_NR_EVAL_INFO_SIZE_V1)
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    const bool v11 = info->struct_size >= offsetof(slip_nr_eval_info, responsivity) + sizeof(slip_nr_image);
    if (f->type == SLIP_NR_FEATURE_SUPER_RESOLUTION)
    {
        if (!info->color.image || !info->output.image || !info->depth.image || !info->motion.image)
        {
            setError("slip_nr_evaluate (Super Resolution): a required image is missing");
            return SLIP_NR_ERROR_INVALID_ARGUMENT;
        }
        NVSDK_NGX_Resource_VK rColor, rOut, rDepth, rMv, rExp, rBias;
        NVSDK_NGX_VK_DLSS_Eval_Params e;
        std::memset(&e, 0, sizeof(e));
        e.Feature.pInColor = wrap(info->color, rColor, false);
        e.Feature.pInOutput = wrap(info->output, rOut, true);
        e.pInDepth = wrap(info->depth, rDepth, false);
        e.pInMotionVectors = wrap(info->motion, rMv, false);
        e.pInExposureTexture = wrap(info->exposure, rExp, false);
        if (v11) e.pInBiasCurrentColorMask = wrap(info->responsivity, rBias, false);
        e.InJitterOffsetX = info->jitter_x;
        e.InJitterOffsetY = info->jitter_y;
        e.InRenderSubrectDimensions.Width = info->render_width ? info->render_width : info->color.width;
        e.InRenderSubrectDimensions.Height = info->render_height ? info->render_height : info->color.height;
        e.InReset = info->reset ? 1 : 0;
        e.InMVScaleX = info->mv_scale_x != 0.f ? info->mv_scale_x : 1.f;
        e.InMVScaleY = info->mv_scale_y != 0.f ? info->mv_scale_y : 1.f;
        e.InPreExposure = info->pre_exposure != 0.f ? info->pre_exposure : 1.f;
        e.InExposureScale = 1.f;
        e.InFrameTimeDeltaInMsec = info->frame_time_ms;
        const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, f->handle, f->params, &e);
        if (NVSDK_NGX_FAILED(r)) return fail("Super Resolution evaluate", r);
        return SLIP_NR_OK;
    }
    if (!info->color.image || !info->output.image || !info->depth.image || !info->motion.image
        || !info->diffuse_albedo.image || !info->specular_albedo.image || !info->normals.image)
    {
        setError("slip_nr_evaluate: a required image is missing");
        return SLIP_NR_ERROR_INVALID_ARGUMENT;
    }
    NVSDK_NGX_Resource_VK rColor, rOut, rDepth, rMv, rDiff, rSpec, rNorm, rRough, rHit, rExp, rBefore, rTransp, rResp;
    NVSDK_NGX_VK_DLSSD_Eval_Params e;
    std::memset(&e, 0, sizeof(e));
    e.pInColor = wrap(info->color, rColor, false);
    e.pInOutput = wrap(info->output, rOut, true);
    e.pInDepth = wrap(info->depth, rDepth, false);
    e.pInMotionVectors = wrap(info->motion, rMv, false);
    e.pInDiffuseAlbedo = wrap(info->diffuse_albedo, rDiff, false);
    e.pInSpecularAlbedo = wrap(info->specular_albedo, rSpec, false);
    e.pInNormals = wrap(info->normals, rNorm, false);
    if (!(f->flags & SLIP_NR_CREATE_ROUGHNESS_PACKED)) e.pInRoughness = wrap(info->roughness, rRough, false);
    e.pInSpecularHitDistance = wrap(info->specular_hit_distance, rHit, false);
    e.pInExposureTexture = wrap(info->exposure, rExp, false);
    e.pInColorBeforeTransparency = wrap(info->color_before_transparency, rBefore, false);
    if (v11)
    {
        e.pInTransparencyLayer = wrap(info->transparency_layer, rTransp, false);
        e.pInResponsivityMask = wrap(info->responsivity, rResp, false);
    }
    e.InJitterOffsetX = info->jitter_x;
    e.InJitterOffsetY = info->jitter_y;
    e.InRenderSubrectDimensions.Width = info->render_width ? info->render_width : info->color.width;
    e.InRenderSubrectDimensions.Height = info->render_height ? info->render_height : info->color.height;
    e.InReset = info->reset ? 1 : 0;
    e.InMVScaleX = info->mv_scale_x != 0.f ? info->mv_scale_x : 1.f;
    e.InMVScaleY = info->mv_scale_y != 0.f ? info->mv_scale_y : 1.f;
    e.InPreExposure = info->pre_exposure != 0.f ? info->pre_exposure : 1.f;
    e.InExposureScale = 1.f;
    e.InFrameTimeDeltaInMsec = info->frame_time_ms;
    // (NGX takes non-const pointers; it only reads them.)
    float w2v[16], v2c[16];
    std::memcpy(w2v, info->world_to_view, sizeof(w2v));
    std::memcpy(v2c, info->view_to_clip, sizeof(v2c));
    if (e.pInSpecularHitDistance)
    {
        e.pInWorldToViewMatrix = w2v;
        e.pInViewToClipMatrix = v2c;
    }
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, f->handle, f->params, &e);
    if (NVSDK_NGX_FAILED(r)) return fail("Ray Reconstruction evaluate", r);
    return SLIP_NR_OK;
}

SLIP_NR_API void SLIP_NR_CALL slip_nr_destroy(slip_nr_feature* f)
{
    if (!f) return;
    if (f->handle) NVSDK_NGX_VULKAN_ReleaseFeature(f->handle);
    if (f->params && f->ownParams) NVSDK_NGX_VULKAN_DestroyParameters(f->params);
    delete f;
}

SLIP_NR_API void SLIP_NR_CALL slip_nr_shutdown(void)
{
    if (g_init)
    {
        if (g_caps) NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
        g_caps = nullptr;
        NVSDK_NGX_VULKAN_Shutdown1(g_device);
    }
    g_init = false;
    g_device = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logFn = nullptr;
    g_logUser = nullptr;
}

SLIP_NR_API const char* SLIP_NR_CALL slip_nr_last_error(void)
{
    return g_lastError.c_str();
}

} // extern "C"
