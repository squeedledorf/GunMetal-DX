/**
 * @file llxr.h
 * @brief The run-time-loaded OpenXR loader (#1073, VR M0): a function table, no link dependency.
 *
 * The viewer never links the OpenXR loader. With VR off (SlipstreamVREnabled 0 and no --vr) this
 * file's code never runs: the library is not opened, no symbol is resolved.
 *
 * Library: on Linux the system libopenxr_loader.so.1 (distribution package), after the viewer's own
 * lib folder; on Windows openxr_loader.dll next to the executable. A 3p package
 * (sky_line/3p-openxr) will ship the loader for Windows and the AppImage later; until then a
 * missing loader simply means "no VR" with a notice.
 *
 * Headers: the Khronos OpenXR SDK headers (1.1.60), vendored under
 * vkgfx/third_party/openxr/include (Apache-2.0 OR MIT, LICENSE.txt there). Compiled with
 * XR_NO_PROTOTYPES: every call goes through this table, so a forgotten entry is a null check, not
 * a link error on a machine without the loader.
 */
#pragma once

#include <vulkan/vulkan.h>

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES 1
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN 1
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <string>

struct LLXrFns
{
    // Bootstrap (the only symbol taken from the library).
    PFN_xrGetInstanceProcAddr getInstanceProcAddr = nullptr;
    // Global (resolved with XR_NULL_HANDLE).
    PFN_xrEnumerateApiLayerProperties enumerateApiLayerProperties = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties enumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance createInstance = nullptr;
    // Instance level (resolved after xrCreateInstance).
    PFN_xrDestroyInstance destroyInstance = nullptr;
    PFN_xrResultToString resultToString = nullptr;
    PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
    PFN_xrGetSystem getSystem = nullptr;
    PFN_xrGetSystemProperties getSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurations enumerateViewConfigurations = nullptr;
    PFN_xrGetViewConfigurationProperties getViewConfigurationProperties = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes enumerateEnvironmentBlendModes = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrCreateSession createSession = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrRequestExitSession requestExitSession = nullptr;
    PFN_xrEnumerateReferenceSpaces enumerateReferenceSpaces = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrCreateSwapchain createSwapchain = nullptr;
    PFN_xrDestroySwapchain destroySwapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    // #1076 input (core 1.0): action sets, interaction-profile bindings, action spaces, haptics.
    PFN_xrStringToPath stringToPath = nullptr;
    PFN_xrPathToString pathToString = nullptr;
    PFN_xrCreateActionSet createActionSet = nullptr;
    PFN_xrDestroyActionSet destroyActionSet = nullptr;
    PFN_xrCreateAction createAction = nullptr;
    PFN_xrDestroyAction destroyAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestInteractionProfileBindings = nullptr;
    PFN_xrAttachSessionActionSets attachSessionActionSets = nullptr;
    PFN_xrGetCurrentInteractionProfile getCurrentInteractionProfile = nullptr;
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStateBoolean getActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat getActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f getActionStateVector2f = nullptr;
    PFN_xrGetActionStatePose getActionStatePose = nullptr;
    PFN_xrCreateActionSpace createActionSpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrApplyHapticFeedback applyHapticFeedback = nullptr;
    // XR_KHR_vulkan_enable2.
    PFN_xrGetVulkanGraphicsRequirements2KHR getVulkanGraphicsRequirements2 = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice2 = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
    // #1075 XR_KHR_visibility_mask (optional: null when the extension is not enabled).
    PFN_xrGetVisibilityMaskKHR getVisibilityMask = nullptr;
};

class LLXrLoader
{
public:
    // Opens the loader library and resolves the global entry points. viewerLibDir: the viewer's
    // own library folder (tried first; empty = skip). False: *why says what is missing.
    static bool load(const std::string& viewerLibDir, std::string& why);
    // After xrCreateInstance: every instance-level entry point (the enable2 ones included).
    // False: *why names the first one missing.
    static bool loadInstance(XrInstance inst, std::string& why);
    static bool loaded() { return sLib != nullptr; }
    static const std::string& libraryPath() { return sLibPath; }
    static const LLXrFns& fn() { return sFns; }
    // Forget the instance-level entry points (after xrDestroyInstance). The library itself stays
    // mapped for the life of the process: the runtime it brought in (SteamVR's vrclient) may keep
    // threads / atexit handlers, and unloading under them is less safe than keeping a few MB.
    static void forgetInstance();

private:
    static void* sLib;
    static std::string sLibPath;
    static LLXrFns sFns;
};

// "XR_ERROR_FORM_FACTOR_UNAVAILABLE" etc. (the runtime's own text when an instance exists).
std::string llxrResultText(XrInstance inst, XrResult r);
// "1.2.3"
std::string llxrVersionText(XrVersion v);
