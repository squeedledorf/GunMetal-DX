/**
 * @file llxr.cpp
 * @brief The run-time-loaded OpenXR loader (#1073): see llxr.h.
 */
#include "linden_common.h"

#include "llxr.h"

#include "llerror.h"

#include <cstdio>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <dlfcn.h>
#endif

void* LLXrLoader::sLib = nullptr;
std::string LLXrLoader::sLibPath;
LLXrFns LLXrLoader::sFns;

namespace
{
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

void* libSym(void* lib, const char* name)
{
#if LL_WINDOWS
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}
} // namespace

bool LLXrLoader::load(const std::string& viewerLibDir, std::string& why)
{
    if (sLib) return true;
    // Candidates: the viewer's own library folder (where the 3p package will put it), then the
    // system's (Linux: the distribution's openxr package; Windows: the executable folder is the
    // viewer lib dir already).
    std::vector<std::string> tries;
#if LL_WINDOWS
    const char* name = "openxr_loader.dll";
    if (!viewerLibDir.empty()) tries.push_back(viewerLibDir + "\\" + name);
    tries.push_back(name);
#else
    const char* name = "libopenxr_loader.so.1";
    if (!viewerLibDir.empty()) tries.push_back(viewerLibDir + "/" + name);
    tries.push_back(name);                    // ld.so search path (the system package)
    tries.push_back("libopenxr_loader.so");   // a development install without the soname link
#endif
    std::string errs;
    for (const std::string& t : tries)
    {
        std::string err;
        void* h = openLib(t, err);
        if (h)
        {
            sLib = h;
            sLibPath = t;
            break;
        }
        errs += (errs.empty() ? "" : "; ") + t + ": " + err;
    }
    if (!sLib)
    {
        why = std::string("the OpenXR loader (") + name + ") was not found (" + errs + ")";
        return false;
    }
    sFns = LLXrFns{};
    sFns.getInstanceProcAddr = (PFN_xrGetInstanceProcAddr)libSym(sLib, "xrGetInstanceProcAddr");
    if (!sFns.getInstanceProcAddr)
    {
        why = sLibPath + " has no xrGetInstanceProcAddr";
        return false;   // (kept mapped: see forgetInstance)
    }
    auto global = [](const char* n) {
        PFN_xrVoidFunction f = nullptr;
        if (XR_FAILED(sFns.getInstanceProcAddr(XR_NULL_HANDLE, n, &f))) f = nullptr;
        return f;
    };
    sFns.enumerateApiLayerProperties = (PFN_xrEnumerateApiLayerProperties)global("xrEnumerateApiLayerProperties");
    sFns.enumerateInstanceExtensionProperties =
        (PFN_xrEnumerateInstanceExtensionProperties)global("xrEnumerateInstanceExtensionProperties");
    sFns.createInstance = (PFN_xrCreateInstance)global("xrCreateInstance");
    if (!sFns.enumerateInstanceExtensionProperties || !sFns.createInstance)
    {
        why = sLibPath + ": the loader's global entry points are missing";
        return false;
    }
    return true;
}

bool LLXrLoader::loadInstance(XrInstance inst, std::string& why)
{
    if (!sFns.getInstanceProcAddr || inst == XR_NULL_HANDLE)
    {
        why = "no OpenXR instance";
        return false;
    }
    std::string missing;
    auto get = [&](const char* n) {
        PFN_xrVoidFunction f = nullptr;
        if (XR_FAILED(sFns.getInstanceProcAddr(inst, n, &f)) || !f)
        {
            f = nullptr;
            if (missing.empty()) missing = n;
        }
        return f;
    };
#define LLXR_GET(member, type, name) sFns.member = (type)get(name)
    LLXR_GET(destroyInstance, PFN_xrDestroyInstance, "xrDestroyInstance");
    LLXR_GET(resultToString, PFN_xrResultToString, "xrResultToString");
    LLXR_GET(getInstanceProperties, PFN_xrGetInstanceProperties, "xrGetInstanceProperties");
    LLXR_GET(getSystem, PFN_xrGetSystem, "xrGetSystem");
    LLXR_GET(getSystemProperties, PFN_xrGetSystemProperties, "xrGetSystemProperties");
    LLXR_GET(enumerateViewConfigurations, PFN_xrEnumerateViewConfigurations, "xrEnumerateViewConfigurations");
    LLXR_GET(getViewConfigurationProperties, PFN_xrGetViewConfigurationProperties, "xrGetViewConfigurationProperties");
    LLXR_GET(enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews, "xrEnumerateViewConfigurationViews");
    LLXR_GET(enumerateEnvironmentBlendModes, PFN_xrEnumerateEnvironmentBlendModes, "xrEnumerateEnvironmentBlendModes");
    LLXR_GET(pollEvent, PFN_xrPollEvent, "xrPollEvent");
    LLXR_GET(createSession, PFN_xrCreateSession, "xrCreateSession");
    LLXR_GET(destroySession, PFN_xrDestroySession, "xrDestroySession");
    LLXR_GET(beginSession, PFN_xrBeginSession, "xrBeginSession");
    LLXR_GET(endSession, PFN_xrEndSession, "xrEndSession");
    LLXR_GET(requestExitSession, PFN_xrRequestExitSession, "xrRequestExitSession");
    LLXR_GET(enumerateReferenceSpaces, PFN_xrEnumerateReferenceSpaces, "xrEnumerateReferenceSpaces");
    LLXR_GET(createReferenceSpace, PFN_xrCreateReferenceSpace, "xrCreateReferenceSpace");
    LLXR_GET(destroySpace, PFN_xrDestroySpace, "xrDestroySpace");
    LLXR_GET(enumerateSwapchainFormats, PFN_xrEnumerateSwapchainFormats, "xrEnumerateSwapchainFormats");
    LLXR_GET(createSwapchain, PFN_xrCreateSwapchain, "xrCreateSwapchain");
    LLXR_GET(destroySwapchain, PFN_xrDestroySwapchain, "xrDestroySwapchain");
    LLXR_GET(enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages, "xrEnumerateSwapchainImages");
    LLXR_GET(acquireSwapchainImage, PFN_xrAcquireSwapchainImage, "xrAcquireSwapchainImage");
    LLXR_GET(waitSwapchainImage, PFN_xrWaitSwapchainImage, "xrWaitSwapchainImage");
    LLXR_GET(releaseSwapchainImage, PFN_xrReleaseSwapchainImage, "xrReleaseSwapchainImage");
    LLXR_GET(waitFrame, PFN_xrWaitFrame, "xrWaitFrame");
    LLXR_GET(beginFrame, PFN_xrBeginFrame, "xrBeginFrame");
    LLXR_GET(endFrame, PFN_xrEndFrame, "xrEndFrame");
    LLXR_GET(locateViews, PFN_xrLocateViews, "xrLocateViews");
    LLXR_GET(stringToPath, PFN_xrStringToPath, "xrStringToPath");
    LLXR_GET(pathToString, PFN_xrPathToString, "xrPathToString");
    LLXR_GET(createActionSet, PFN_xrCreateActionSet, "xrCreateActionSet");
    LLXR_GET(destroyActionSet, PFN_xrDestroyActionSet, "xrDestroyActionSet");
    LLXR_GET(createAction, PFN_xrCreateAction, "xrCreateAction");
    LLXR_GET(destroyAction, PFN_xrDestroyAction, "xrDestroyAction");
    LLXR_GET(suggestInteractionProfileBindings, PFN_xrSuggestInteractionProfileBindings, "xrSuggestInteractionProfileBindings");
    LLXR_GET(attachSessionActionSets, PFN_xrAttachSessionActionSets, "xrAttachSessionActionSets");
    LLXR_GET(getCurrentInteractionProfile, PFN_xrGetCurrentInteractionProfile, "xrGetCurrentInteractionProfile");
    LLXR_GET(syncActions, PFN_xrSyncActions, "xrSyncActions");
    LLXR_GET(getActionStateBoolean, PFN_xrGetActionStateBoolean, "xrGetActionStateBoolean");
    LLXR_GET(getActionStateFloat, PFN_xrGetActionStateFloat, "xrGetActionStateFloat");
    LLXR_GET(getActionStateVector2f, PFN_xrGetActionStateVector2f, "xrGetActionStateVector2f");
    LLXR_GET(getActionStatePose, PFN_xrGetActionStatePose, "xrGetActionStatePose");
    LLXR_GET(createActionSpace, PFN_xrCreateActionSpace, "xrCreateActionSpace");
    LLXR_GET(locateSpace, PFN_xrLocateSpace, "xrLocateSpace");
    LLXR_GET(applyHapticFeedback, PFN_xrApplyHapticFeedback, "xrApplyHapticFeedback");
    LLXR_GET(getVulkanGraphicsRequirements2, PFN_xrGetVulkanGraphicsRequirements2KHR, "xrGetVulkanGraphicsRequirements2KHR");
    LLXR_GET(createVulkanInstance, PFN_xrCreateVulkanInstanceKHR, "xrCreateVulkanInstanceKHR");
    LLXR_GET(getVulkanGraphicsDevice2, PFN_xrGetVulkanGraphicsDevice2KHR, "xrGetVulkanGraphicsDevice2KHR");
    LLXR_GET(createVulkanDevice, PFN_xrCreateVulkanDeviceKHR, "xrCreateVulkanDeviceKHR");
#undef LLXR_GET
    // Optional entry points (extensions the instance may not have enabled): never "missing".
    {
        PFN_xrVoidFunction f = nullptr;
        sFns.getVisibilityMask = (XR_SUCCEEDED(sFns.getInstanceProcAddr(inst, "xrGetVisibilityMaskKHR", &f)) && f)
                               ? (PFN_xrGetVisibilityMaskKHR)f : nullptr;
    }
    if (!missing.empty())
    {
        why = "the OpenXR runtime does not provide " + missing;
        return false;
    }
    return true;
}

void LLXrLoader::forgetInstance()
{
    PFN_xrGetInstanceProcAddr gipa = sFns.getInstanceProcAddr;
    PFN_xrEnumerateApiLayerProperties layers = sFns.enumerateApiLayerProperties;
    PFN_xrEnumerateInstanceExtensionProperties exts = sFns.enumerateInstanceExtensionProperties;
    PFN_xrCreateInstance create = sFns.createInstance;
    sFns = LLXrFns{};
    sFns.getInstanceProcAddr = gipa;
    sFns.enumerateApiLayerProperties = layers;
    sFns.enumerateInstanceExtensionProperties = exts;
    sFns.createInstance = create;
}

std::string llxrResultText(XrInstance inst, XrResult r)
{
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (inst != XR_NULL_HANDLE && LLXrLoader::fn().resultToString &&
        XR_SUCCEEDED(LLXrLoader::fn().resultToString(inst, r, buf)) && buf[0])
        return buf;
    switch (r)
    {
    case XR_SUCCESS: return "XR_SUCCESS";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
    default: break;
    }
    return "XrResult(" + std::to_string((int)r) + ")";
}

std::string llxrVersionText(XrVersion v)
{
    return std::to_string(XR_VERSION_MAJOR(v)) + "." + std::to_string(XR_VERSION_MINOR(v)) + "." +
           std::to_string(XR_VERSION_PATCH(v));
}
