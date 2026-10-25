/**
 * @file vk_requirements.h
 * @brief Slipstream's Vulkan minimum: per-device requirements check, device ranking,
 *        and the plain-language "this computer can't run Slipstream" text (#526).
 *
 * ONE definition of what the renderer needs from a physical device: Vulkan 1.3 -- or (#675)
 * Vulkan 1.2 plus the extensions 1.3 promoted that the renderer uses (VK_KHR_dynamic_rendering,
 * VK_KHR_synchronization2, VK_EXT_extended_dynamic_state, VK_EXT_shader_demote_to_helper_invocation)
 * -- the core / 1.2 / 1.3 feature bits createLogicalDevice enables unconditionally, the device
 * extensions it always enables, and a graphics queue. Optional features (wide lines,
 * depth clamp, BC textures, VRS, ray tracing ...) are probed in createLogicalDevice and
 * are NOT requirements.
 *
 * Used twice:
 *  - vkCheckRequirements(): the viewer's pre-flight, BEFORE the viewer window exists. A
 *    throwaway instance (no layers, no surface extensions) enumerates every device; when
 *    none qualifies (or there is no Vulkan driver at all) the viewer shows
 *    vkRequirementsUserMessage() in a native message box and exits cleanly.
 *  - VkDeviceContext::pickPhysicalDevice(): picks the best QUALIFYING device (discrete,
 *    then integrated, then virtual, then anything else; first enumerated wins a tie), so
 *    a hybrid laptop with an old iGPU driver but a capable dGPU (or the reverse) runs.
 *
 * No LL dependencies (logging is the caller's): strings only.
 *
 * TEST HOOK: env SLIPSTREAM_VK_SIMULATE_UNSUPPORTED (read on every evaluation):
 *   1 | api      every device reports Vulkan 1.1 (below the 1.2 minimum, #675; the #526 case)
 *   feature      every device misses drawIndirectCount
 *   discrete     only discrete GPUs fail (feature) -> a hybrid machine picks the iGPU
 *   nodevice     the driver reports no devices
 *   translation  (#950) every device is treated as a translation layer (driver-check dialog)
 *   missingdriver (#950) adds a fake discrete NVIDIA adapter with no Vulkan driver (dialog)
 *   noinstance   vkCreateInstance fails with VK_ERROR_INCOMPATIBLE_DRIVER (no driver)
 * Simulated items are marked "(simulated)" in the log and the dialog.
 */
#pragma once

#include "vk_common.h"

#include <string>
#include <vector>

// The device extensions createLogicalDevice always enables (promoted to core in 1.2 /
// 1.3, but still requested by name), plus VK_KHR_swapchain when presenting.
// api12 = the Vulkan 1.2 path (#675): also the 1.3-promoted extensions the 1.3 core provides.
std::vector<const char*> vkRequiredDeviceExtensions(bool wantSwapchain, bool api12 = false);

// #675: true makes every device use the Vulkan 1.2 + extensions path even when it reports
// 1.3 (RenderVKForceApi12, or env SLIPSTREAM_VK_FORCE_API12=1), so the fallback can be
// tested on a current GPU. Set before vkCheckRequirements / the device context is created.
extern bool gVkForceApi12;

struct VkGpuRequirements
{
    VkPhysicalDevice device = VK_NULL_HANDLE;
    std::string name;
    uint32_t vendorID = 0;
    uint32_t deviceID = 0;
    uint32_t driverVersion = 0;
    uint32_t apiVersion = 0;               // as reported (before any simulation)
    bool useApi12 = false;                 // #675: the 1.2 + extensions path (device < 1.3, or forced)
    std::vector<std::string> caps;         // #675: capability check lines (1.2 path only; logged)
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::string driverText;                // decoded driverVersion (+ driverInfo when known)
    std::vector<std::string> missing;      // empty = qualifies
    // #277: stable identifier for the GPU choice setting (RenderVKDeviceSelect): the
    // VkPhysicalDeviceIDProperties deviceUUID in hex, or "vvvv-dddd[-n]" (vendor-device id plus
    // an index among identical cards) when the driver reports no UUID. Not an enumeration index.
    std::string id;
    uint32_t driverID = 0;                 // VkDriverId (0 = unknown / pre-1.2 driver)
    int cls = 0;                           // VkDeviceClass (vkClassifyDevice): real driver / translation layer / software
    uint64_t vramBytes = 0;                // size of the largest device-local memory heap (0 = unknown)

    bool qualifies() const { return missing.empty(); }
};

struct VkRequirementsReport
{
    bool instanceOk = false;               // a Vulkan instance could be created
    VkResult instanceResult = VK_SUCCESS;  // vkCreateInstance / enumerate result when not
    uint32_t loaderVersion = 0;            // vkEnumerateInstanceVersion (0 = unknown)
    std::vector<VkGpuRequirements> gpus;
    int best = -1;                         // index into gpus of the chosen device, -1 = none
    std::string bestWhy;                   // why it was chosen (log)
    std::string failure;                   // a later bring-up step that failed (dialog + log)
    std::string preferenceNote;            // #277: the saved GPU choice could not be honoured (user-visible, "" = fine)

    bool ok() const { return instanceOk && best >= 0; }
};

// #950: what answers for a Vulkan device. Anything but Real is a device that is NOT the
// computer's own GPU driver (it renders, but often wrongly -- the "every texture white" case).
enum VkDeviceClass
{
    VK_DEVICE_REAL = 0,
    VK_DEVICE_TRANSLATION = 1,   // Microsoft Dozen (Vulkan on D3D12), other layered implementations
    VK_DEVICE_SOFTWARE = 2       // lavapipe / llvmpipe, SwiftShader, VK_PHYSICAL_DEVICE_TYPE_CPU
};
// Classifies by driverID (MESA_DOZEN 23, MESA_LLVMPIPE 13, GOOGLE_SWIFTSHADER 10), the device
// type, and the device name ("Microsoft Direct3D12", llvmpipe, lavapipe, SwiftShader).
VkDeviceClass vkClassifyDevice(const std::string& name, VkPhysicalDeviceType type, uint32_t driverID);
const char* vkDeviceClassName(VkDeviceClass c);

// A graphics adapter the OS knows about (Windows: the DXGI adapter list; empty elsewhere).
// Used to spot a discrete GPU that has no Vulkan driver installed at all.
struct VkOsAdapter
{
    std::string name;
    uint32_t vendorID = 0;
    uint64_t vramBytes = 0;     // dedicated video memory
    bool software = false;      // Microsoft Basic Render Driver / WARP
};
std::vector<VkOsAdapter> vkEnumerateOsAdapters();

enum class VkSetupIssue
{
    None,
    Translation,               // chosen device is a translation layer (Dozen ...)
    Software,                  // chosen device is a software renderer
    IntegratedWhileDiscrete,   // chosen an integrated GPU although a discrete one exists
    DriverNotInstalled,        // a discrete GPU the OS knows has no Vulkan driver at all (Windows DXGI)
    DiscreteUnusable           // a discrete GPU's Vulkan driver exists but does not meet the minimum (too old)
};
const char* vkSetupIssueName(VkSetupIssue i);

struct VkDriverCheck
{
    VkSetupIssue issue = VkSetupIssue::None;
    std::string realGpuName;   // the real GPU, when known ("" = unknown)
    std::string realVendor;    // "NVIDIA" / "AMD" / "Intel" or ""
    std::string key;           // remembered by "Don't show this again": name | raw driver | vendor
    std::string logText;       // classification + device details (no user data)
    std::string userText;      // plain-language dialog body
};
// Classifies the chosen device of `r` (r.ok()). `userChose` = the GPU came from the explicit
// graphics card setting, which is respected: the integrated-while-discrete case is not raised.
// `osAdapters` = vkEnumerateOsAdapters() (injectable for tests).
VkDriverCheck vkClassifyDriverSetup(const VkRequirementsReport& r, const std::vector<VkOsAdapter>& osAdapters, bool userChose);
// The dialog title.
const char* vkDriverCheckTitle();

// #277 GPU selection. "" or "auto" = Automatic (the ranking below). Otherwise a
// VkGpuRequirements::id: that device is used when it qualifies; when it is missing or does
// not meet the requirements the choice falls back to Automatic and VkRequirementsReport::
// preferenceNote says why (the saved setting is left alone, so it applies again when the
// device comes back). Set before vkCheckRequirements / the device context is brought up.
void vkSetPreferredDevice(const std::string& id);
// The report of the most recent vkCheckRequirements() (the viewer pre-flight), for the
// graphics card lists in Preferences / login. Empty gpus before the pre-flight ran.
const VkRequirementsReport& vkLastRequirementsReport();
// Returns the pending "your GPU choice could not be used" text once (then ""), so the
// viewer can show it as a notification when the UI is up.
std::string vkTakePreferenceNote();

// Evaluates one device against the requirements.
VkGpuRequirements vkEvaluateDevice(VkPhysicalDevice dev, bool wantSwapchain);
// Enumerates + evaluates every device of `instance` and ranks the qualifying ones
// (fills gpus / best / bestWhy; instanceOk / instanceResult describe the enumeration).
void vkEvaluateDevices(VkInstance instance, bool wantSwapchain, VkRequirementsReport& out);
// The pre-flight: throwaway 1.3 instance, evaluate (with the swapchain extension), destroy.
VkRequirementsReport vkCheckRequirements();

// "560.94" (NVIDIA), "100.8142" (Intel on Windows), "24.2.3" (Mesa / AMD / others).
std::string vkDecodeDriverVersion(uint32_t vendorID, uint32_t driverVersion, uint32_t driverID);
const char* vkVendorName(uint32_t vendorID);
const char* vkDeviceTypeName(VkPhysicalDeviceType t);
// One "Vulkan requirements: ..." log line per device.
std::string vkRequirementsLogLine(const VkGpuRequirements& g);
// Whole-report log lines (instance / loader, every device, the choice).
std::vector<std::string> vkRequirementsLogLines(const VkRequirementsReport& r);
// The message box title and text shown when Slipstream can't run on this computer.
const char* vkRequirementsUserTitle();
std::string vkRequirementsUserMessage(const VkRequirementsReport& r);
// SLIPSTREAM_VK_SIMULATE_UNSUPPORTED value ("" = off).
std::string vkSimulateUnsupportedMode();

extern const char* const kVkRequirementsHelpURL;
