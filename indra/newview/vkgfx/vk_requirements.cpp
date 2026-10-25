/**
 * @file vk_requirements.cpp
 * @brief See vk_requirements.h (#526).
 */
#include "vk_requirements.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dxgi.h>
#endif

const char* const kVkRequirementsHelpURL = "https://slipstreamviewer.org/wiki/graphics-requirements/";

namespace
{
constexpr uint32_t kVendorAMD    = 0x1002;
constexpr uint32_t kVendorNVIDIA = 0x10DE;
constexpr uint32_t kVendorIntel  = 0x8086;

std::string versionText(uint32_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v));
    return buf;
}

bool envSet(const char* name)
{
    const char* v = ::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
} // namespace

std::string vkSimulateUnsupportedMode()
{
    const char* v = ::getenv("SLIPSTREAM_VK_SIMULATE_UNSUPPORTED");
    if (!v || !*v || std::strcmp(v, "0") == 0) return std::string();
    if (std::strcmp(v, "1") == 0) return "api";
    return v;
}

bool gVkForceApi12 = false;

std::vector<const char*> vkRequiredDeviceExtensions(bool wantSwapchain, bool api12)
{
    std::vector<const char*> e = {
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    };
    if (wantSwapchain) e.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (api12)
    {
        // #675: what Vulkan 1.3 folds into core and the renderer uses (the device is < 1.3 or
        // forced onto this path). dynamic_rendering / synchronization2 are already above.
        e.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        e.push_back(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME);
    }
    return e;
}

const char* vkVendorName(uint32_t vendorID)
{
    switch (vendorID)
    {
    case kVendorAMD:    return "AMD";
    case kVendorNVIDIA: return "NVIDIA";
    case kVendorIntel:  return "Intel";
    case 0x13B5:        return "ARM";
    case 0x5143:        return "Qualcomm";
    case 0x106B:        return "Apple";
    case 0x10005:       return "Mesa";
    case 0x1414:        return "Microsoft";
    default:            return "unknown vendor";
    }
}

VkDeviceClass vkClassifyDevice(const std::string& name, VkPhysicalDeviceType type, uint32_t driverID)
{
    std::string n;
    for (char c : name) n += (char)std::tolower((unsigned char)c);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    // VK_DRIVER_ID_GOOGLE_SWIFTSHADER = 10, VK_DRIVER_ID_MESA_LLVMPIPE = 13 (literals: older
    // vendored Vulkan headers may lack the newer enumerants).
    if (driverID == 10 || driverID == 13 || type == VK_PHYSICAL_DEVICE_TYPE_CPU ||
        has("llvmpipe") || has("lavapipe") || has("swiftshader"))
        return VK_DEVICE_SOFTWARE;
    // VK_DRIVER_ID_MESA_DOZEN = 23: Vulkan on top of Direct3D 12.
    if (driverID == 23 || has("microsoft direct3d12") || has("direct3d12") || has("dozen"))
        return VK_DEVICE_TRANSLATION;
    return VK_DEVICE_REAL;
}

const char* vkDeviceClassName(VkDeviceClass c)
{
    switch (c)
    {
    case VK_DEVICE_TRANSLATION: return "translation layer";
    case VK_DEVICE_SOFTWARE:    return "software renderer";
    default:                    return "real driver";
    }
}

const char* vkSetupIssueName(VkSetupIssue i)
{
    switch (i)
    {
    case VkSetupIssue::Translation:             return "translation-layer";
    case VkSetupIssue::Software:                return "software-renderer";
    case VkSetupIssue::IntegratedWhileDiscrete: return "integrated-while-discrete";
    case VkSetupIssue::DriverNotInstalled:      return "discrete-gpu-without-vulkan-driver";
    case VkSetupIssue::DiscreteUnusable:        return "discrete-gpu-driver-too-old";
    default:                                    return "none";
    }
}

std::vector<VkOsAdapter> vkEnumerateOsAdapters()
{
    std::vector<VkOsAdapter> out;
#if defined(_WIN32)
    // dxgi.dll is loaded dynamically: no new link dependency, and a missing / broken DXGI
    // simply yields an empty list.
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    if (!m) return out;
    typedef HRESULT (WINAPI *CreateFactory1Fn)(REFIID, void**);
    CreateFactory1Fn create = (CreateFactory1Fn)(void*)GetProcAddress(m, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&f)) && f)
    {
        for (UINT i = 0;; ++i)
        {
            IDXGIAdapter1* a = nullptr;
            if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d)))
            {
                VkOsAdapter o;
                char buf[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, buf, (int)sizeof(buf) - 1, nullptr, nullptr);
                o.name = buf;
                o.vendorID = d.VendorId;
                o.vramBytes = (uint64_t)d.DedicatedVideoMemory;
                o.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || d.VendorId == 0x1414;
                out.push_back(o);
            }
            a->Release();
        }
        f->Release();
    }
    FreeLibrary(m);
#endif
    if (vkSimulateUnsupportedMode() == "missingdriver")
    {
        VkOsAdapter o;
        o.name = "NVIDIA GeForce RTX 5050 Laptop GPU (simulated)";
        o.vendorID = kVendorNVIDIA;
        o.vramBytes = 8ull << 30;
        out.push_back(o);
    }
    return out;
}

const char* vkDeviceTypeName(VkPhysicalDeviceType t)
{
    switch (t)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "software (CPU)";
    default:                                     return "other";
    }
}

std::string vkDecodeDriverVersion(uint32_t vendorID, uint32_t v, uint32_t driverID)
{
    char buf[48];
    if (vendorID == kVendorNVIDIA && driverID != VK_DRIVER_ID_MESA_NVK)
    {
        // NVIDIA proprietary: 10.8.8.6 bits.
        const uint32_t major = (v >> 22) & 0x3ff, minor = (v >> 14) & 0xff, sec = (v >> 6) & 0xff;
        if (sec)
            std::snprintf(buf, sizeof(buf), "%u.%02u.%02u", major, minor, sec);
        else
            std::snprintf(buf, sizeof(buf), "%u.%02u", major, minor);
        return buf;
    }
    bool intelWin = driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS;
#if defined(_WIN32)
    if (driverID == 0 && vendorID == kVendorIntel) intelWin = true;   // pre-1.2 driver: no driver ID
#endif
    if (intelWin)
    {
        // Intel Windows: 18.14 bits -- the last two parts of the "31.0.101.2130" version.
        std::snprintf(buf, sizeof(buf), "%u.%u", v >> 14, v & 0x3fff);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    return buf;
}

VkGpuRequirements vkEvaluateDevice(VkPhysicalDevice dev, bool wantSwapchain)
{
    VkGpuRequirements g;
    g.device = dev;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(dev, &p);
    g.name = p.deviceName;
    g.vendorID = p.vendorID;
    g.deviceID = p.deviceID;
    g.driverVersion = p.driverVersion;
    g.apiVersion = p.apiVersion;
    g.type = p.deviceType;

    // #277: stable identifier + VRAM size for the GPU choice.
    {
        std::string uuid;
        if (p.apiVersion >= VK_API_VERSION_1_1)
        {
            VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &idp;
            vkGetPhysicalDeviceProperties2(dev, &p2);
            static const char kHex[] = "0123456789abcdef";
            for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
            {
                uuid += kHex[(idp.deviceUUID[i] >> 4) & 15u];
                uuid += kHex[idp.deviceUUID[i] & 15u];
            }
            if (uuid.find_first_not_of('0') == std::string::npos) uuid.clear();   // all zero: not reported
        }
        if (uuid.empty())
        {
            char vd[32];
            std::snprintf(vd, sizeof(vd), "%04x-%04x", p.vendorID, p.deviceID);
            uuid = vd;
        }
        g.id = uuid;

        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(dev, &mp);
        for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
            if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > g.vramBytes)
                g.vramBytes = mp.memoryHeaps[i].size;
    }

    const std::string sim = vkSimulateUnsupportedMode();
    const char* simTag = " (simulated)";
    uint32_t api = p.apiVersion;
    bool simApi = false;
    if (sim == "api" && api >= VK_API_VERSION_1_2)
    {
        api = VK_API_VERSION_1_1;   // simulate a driver below the 1.2 minimum (#675)
        simApi = true;
    }
    // #675: a device that reports >= 1.3 (and is not forced) takes the unchanged 1.3 path;
    // 1.2 devices (and forced ones) take 1.2 + the promoted extensions.
    const bool forced12 = (gVkForceApi12 || envSet("SLIPSTREAM_VK_FORCE_API12")) && api >= VK_API_VERSION_1_3;
    const bool api12 = api < VK_API_VERSION_1_3 || forced12;
    g.useApi12 = api12 && api >= VK_API_VERSION_1_2;

    // Driver identity (VkPhysicalDeviceDriverProperties is core 1.2).
    uint32_t driverID = 0;
    std::string driverInfo;
    if (p.apiVersion >= VK_API_VERSION_1_2)
    {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dev, &p2);
        driverID = (uint32_t)drv.driverID;
        driverInfo = drv.driverInfo;
    }
    g.driverID = driverID;
    {   // #950: a translation layer / software renderer is not the computer's own GPU driver.
        int c = (int)vkClassifyDevice(g.name, p.deviceType, driverID);
        if (sim == "translation") c = VK_DEVICE_TRANSLATION;
        g.cls = c;
    }
    g.driverText = vkDecodeDriverVersion(p.vendorID, p.driverVersion, driverID);
    if (!driverInfo.empty() && driverInfo != g.driverText) g.driverText += " [" + driverInfo + "]";

    if (api < VK_API_VERSION_1_2)
        g.missing.push_back("Vulkan 1.2 or newer (the driver provides " + versionText(api) + ")" + (simApi ? simTag : ""));
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !envSet("SLIPSTREAM_VK_ALLOW_CPU"))
        g.missing.push_back("a hardware GPU (this is a software renderer)");

    // Device extensions (listed first: the 1.2 path chains the extension feature structs only
    // for extensions the device lists).
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ep(en);
    if (en) vkEnumerateDeviceExtensionProperties(dev, nullptr, &en, ep.data());
    auto listed = [&](const char* name)
    {
        for (uint32_t i = 0; i < en; ++i)
            if (std::strcmp(ep[i].extensionName, name) == 0) return true;
        return false;
    };

    // Feature bits createLogicalDevice enables unconditionally. Only chain the 1.2 / 1.3
    // structs the device's version knows (zero-initialized: unfilled reads as missing).
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    // #675 (1.2 path): the extension feature structs that stand in for Vulkan13Features.
    VkPhysicalDeviceDynamicRenderingFeaturesKHR xDyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR xSync{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT xEds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT xDem{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    if (p.apiVersion >= VK_API_VERSION_1_1)
    {
        if (p.apiVersion >= VK_API_VERSION_1_2) { f2.pNext = &f12; f12.pNext = &f11; }
        if (p.apiVersion >= VK_API_VERSION_1_3) f11.pNext = &f13;
        if (api12 && p.apiVersion >= VK_API_VERSION_1_2)
        {
            void** tail = (p.apiVersion >= VK_API_VERSION_1_3) ? &f13.pNext : &f11.pNext;
            auto chain = [&](bool have, auto& s)
            {
                if (!have) return;
                *tail = &s;
                tail = &s.pNext;
            };
            chain(listed(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME), xDyn);
            chain(listed(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME), xSync);
            chain(listed(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME), xEds);
            chain(listed(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME), xDem);
        }
        vkGetPhysicalDeviceFeatures2(dev, &f2);
    }
    else
    {
        vkGetPhysicalDeviceFeatures(dev, &f2.features);
    }
    auto need = [&](VkBool32 have, const char* what)
    {
        if (!have) g.missing.push_back(std::string("feature ") + what);
    };
    need(f2.features.multiDrawIndirect, "multiDrawIndirect");
    need(f2.features.drawIndirectFirstInstance, "drawIndirectFirstInstance");
    need(f2.features.samplerAnisotropy, "samplerAnisotropy");
    need(f2.features.imageCubeArray, "imageCubeArray");
    // #704: the GPU cursor pick (fragment list from the raster, clean_pick.frag) is the ONLY
    // pick -- there is no CPU narrowphase to fall back to. Universal on desktop drivers.
    need(f2.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics");
    if (api >= VK_API_VERSION_1_2)
    {
        need(f12.bufferDeviceAddress, "bufferDeviceAddress");
        need(f12.descriptorIndexing, "descriptorIndexing");
        need(f12.runtimeDescriptorArray, "runtimeDescriptorArray");
        need(f12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
        need(f12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
        need(f12.descriptorBindingVariableDescriptorCount, "descriptorBindingVariableDescriptorCount");
        need(f12.timelineSemaphore, "timelineSemaphore");
        need(f12.drawIndirectCount, "drawIndirectCount");
        need(f11.shaderDrawParameters, "shaderDrawParameters");   // quantized geometry (#573)
    }
    if (!api12 && api >= VK_API_VERSION_1_3)
    {
        need(f13.dynamicRendering, "dynamicRendering");
        need(f13.synchronization2, "synchronization2");
    }
    if (api12 && api >= VK_API_VERSION_1_2)
    {
        // A missing extension is reported once, by the extension check below.
        if (listed(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) need(xDyn.dynamicRendering, "dynamicRendering (VK_KHR_dynamic_rendering)");
        if (listed(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) need(xSync.synchronization2, "synchronization2 (VK_KHR_synchronization2)");
        if (listed(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)) need(xEds.extendedDynamicState, "extendedDynamicState (VK_EXT_extended_dynamic_state)");
        if (listed(VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME))
            need(xDem.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation (VK_EXT_shader_demote_to_helper_invocation)");
    }
    if (sim == "feature" || (sim == "discrete" && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU))
        g.missing.push_back(std::string("feature drawIndirectCount") + simTag);

    // Device extensions.
    for (const char* want : vkRequiredDeviceExtensions(wantSwapchain, api12))
    {
        const bool have = listed(want);
        if (!have) g.missing.push_back(std::string("extension ") + want);
        if (api12) g.caps.push_back(std::string("required extension ") + want + (have ? ": present" : ": MISSING"));
    }
    if (api12)
    {
        // Optional on this path (logged so a report shows what the driver offers).
        for (const char* opt : {VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_EXT_IMAGE_ROBUSTNESS_EXTENSION_NAME})
            g.caps.push_back(std::string("optional extension ") + opt + (listed(opt) ? ": present" : ": absent"));
    }

    // A graphics (+compute) queue family.
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    if (qn) vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qf.data());
    bool gfx = false;
    for (const auto& q : qf)
        if ((q.queueFlags & VK_QUEUE_GRAPHICS_BIT) && (q.queueFlags & VK_QUEUE_COMPUTE_BIT) && q.queueCount) gfx = true;
    if (!gfx) g.missing.push_back("a graphics + compute queue");
    return g;
}

static std::string gPreferredDevice;            // "" = Automatic
static VkRequirementsReport gLastReport;
static std::string gPendingPreferenceNote;

void vkSetPreferredDevice(const std::string& id)
{
    gPreferredDevice = (id == "auto") ? std::string() : id;
}

const VkRequirementsReport& vkLastRequirementsReport()
{
    return gLastReport;
}

std::string vkTakePreferenceNote()
{
    std::string s;
    s.swap(gPendingPreferenceNote);
    return s;
}

void vkEvaluateDevices(VkInstance instance, bool wantSwapchain, VkRequirementsReport& out)
{
    out.gpus.clear();
    out.best = -1;
    out.bestWhy.clear();
    out.preferenceNote.clear();
    uint32_t n = 0;
    VkResult vr = vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs;
    if (vr == VK_SUCCESS && n)
    {
        devs.resize(n);
        vr = vkEnumeratePhysicalDevices(instance, &n, devs.data());
        if (vr == VK_INCOMPLETE) vr = VK_SUCCESS;
        devs.resize(n);
    }
    if (vr != VK_SUCCESS)
    {
        out.instanceResult = vr;
        return;
    }
    if (vkSimulateUnsupportedMode() == "nodevice") devs.clear();
    for (VkPhysicalDevice d : devs)
        out.gpus.push_back(vkEvaluateDevice(d, wantSwapchain));
    // Identical ids (two identical cards without a reported UUID) get an index suffix.
    {
        std::vector<std::string> base;
        for (const VkGpuRequirements& g : out.gpus) base.push_back(g.id);
        for (size_t i = 1; i < out.gpus.size(); ++i)
        {
            int dup = 0;
            for (size_t j = 0; j < i; ++j)
                if (base[j] == base[i]) ++dup;
            if (dup) out.gpus[i].id += "-" + std::to_string(dup);
        }
    }

    // Rank the qualifying devices: discrete > integrated > virtual > other > CPU; the
    // first enumerated wins a tie (keeps the loader / Mesa device-select order).
    // #950: a translation layer / software device ranks below every real-driver device, so a
    // Dozen "discrete" never beats the real GPU's own driver (it is still chosen when it is the
    // only device that qualifies).
    auto rank = [](const VkGpuRequirements& g)
    {
        if (g.cls != VK_DEVICE_REAL) return 5;
        switch (g.type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 4;
        default:                                     return 3;
        }
    };
    int skipped = 0;
    for (int i = 0; i < (int)out.gpus.size(); ++i)
    {
        if (!out.gpus[i].qualifies()) { ++skipped; continue; }
        if (out.best < 0 || rank(out.gpus[i]) < rank(out.gpus[out.best])) out.best = i;
    }
    // #277: the GPU chosen in Preferences / login wins when it qualifies.
    if (!gPreferredDevice.empty() && !out.gpus.empty())
    {
        int pref = -1;
        for (int i = 0; i < (int)out.gpus.size(); ++i)
            if (out.gpus[i].id == gPreferredDevice) { pref = i; break; }
        if (pref >= 0 && out.gpus[pref].qualifies())
        {
            out.best = pref;
            out.bestWhy = "device " + std::to_string(pref) + " '" + out.gpus[pref].name +
                          "' (" + vkDeviceTypeName(out.gpus[pref].type) + " GPU): chosen in the graphics card setting (RenderVKDeviceSelect)";
        }
        else
        {
            if (pref < 0)
                out.preferenceNote = "The graphics card chosen in the settings is not available (device " + gPreferredDevice +
                                     "). Slipstream is using Automatic selection instead; your choice is kept for when it is back.";
            else
                out.preferenceNote = "The graphics card chosen in the settings (" + out.gpus[pref].name +
                                     ") does not meet Slipstream's requirements (" +
                                     (out.gpus[pref].missing.empty() ? std::string("unknown") : out.gpus[pref].missing.front()) +
                                     "). Slipstream is using Automatic selection instead.";
            gPendingPreferenceNote = out.preferenceNote;
        }
    }
    if (out.best >= 0 && out.bestWhy.empty())
    {
        const VkGpuRequirements& b = out.gpus[out.best];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "device %d '%s' (%s GPU): the most capable type that meets the requirements%s",
                      out.best, b.name.c_str(), vkDeviceTypeName(b.type),
                      skipped ? "; skipped devices that do not" : "");
        out.bestWhy = buf;
    }
}

VkRequirementsReport vkCheckRequirements()
{
    VkRequirementsReport r;
    uint32_t loader = 0;
    if (vkEnumerateInstanceVersion(&loader) == VK_SUCCESS) r.loaderVersion = loader;
    if (vkSimulateUnsupportedMode() == "noinstance")
    {
        r.instanceResult = VK_ERROR_INCOMPATIBLE_DRIVER;
        return r;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "slipstream";
    app.pEngineName = "slipstream-vk";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    r.instanceResult = vkCreateInstance(&ci, nullptr, &inst);
    if (r.instanceResult != VK_SUCCESS) return r;
    r.instanceOk = true;
    vkEvaluateDevices(inst, /*wantSwapchain=*/true, r);
    if (r.instanceResult != VK_SUCCESS) r.instanceOk = false;
    vkDestroyInstance(inst, nullptr);
    gLastReport = r;
    return r;
}

std::string vkRequirementsLogLine(const VkGpuRequirements& g)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Vulkan requirements: '%s' (%s GPU, vendor 0x%04X %s, device 0x%04X) driver %s (raw 0x%08X), "
                  "Vulkan %u.%u.%u: ",
                  g.name.c_str(), vkDeviceTypeName(g.type), g.vendorID, vkVendorName(g.vendorID), g.deviceID,
                  g.driverText.c_str(), g.driverVersion, VK_VERSION_MAJOR(g.apiVersion),
                  VK_VERSION_MINOR(g.apiVersion), VK_VERSION_PATCH(g.apiVersion));
    std::string s = buf;
    if (g.useApi12) s += "[Vulkan 1.2 + extensions path] ";
    if (g.qualifies())
        return s + "OK";
    s += "MISSING ";
    for (size_t i = 0; i < g.missing.size(); ++i)
        s += (i ? ", " : "") + g.missing[i];
    return s;
}

std::vector<std::string> vkRequirementsLogLines(const VkRequirementsReport& r)
{
    std::vector<std::string> out;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Vulkan requirements: loader %s, instance %s (%s), %zu device(s)%s",
                  r.loaderVersion ? versionText(r.loaderVersion).c_str() : "unknown",
                  r.instanceOk ? "OK" : "FAILED", vkResultString(r.instanceResult), r.gpus.size(),
                  vkSimulateUnsupportedMode().empty() ? "" : " -- SLIPSTREAM_VK_SIMULATE_UNSUPPORTED is set");
    out.push_back(buf);
    for (const VkGpuRequirements& g : r.gpus)
    {
        out.push_back(vkRequirementsLogLine(g));
        for (const std::string& c : g.caps) out.push_back("Vulkan requirements:   '" + g.name + "' " + c);
    }
    if (r.best >= 0)
        out.push_back("Vulkan requirements: chose " + r.bestWhy);
    else if (r.instanceOk)
        out.push_back("Vulkan requirements: NO device meets Slipstream's minimum (Vulkan 1.3, or 1.2 with the extensions listed above, plus the required features)");
    if (!r.failure.empty())
        out.push_back("Vulkan requirements: bring-up failed: " + r.failure);
    return out;
}

const char* vkRequirementsUserTitle()
{
    return "Slipstream can't start: graphics not supported";
}

std::string vkRequirementsUserMessage(const VkRequirementsReport& r)
{
    std::string s;
    if (!r.instanceOk)
    {
        s = "Slipstream needs a graphics driver with Vulkan support (Vulkan 1.3, or Vulkan 1.2 with dynamic rendering), but no working Vulkan driver was found";
        s += std::string(" (") + vkResultString(r.instanceResult) + ").\n";
    }
    else if (r.gpus.empty())
    {
        s = "Slipstream needs a graphics card with Vulkan support (Vulkan 1.3, or Vulkan 1.2 with dynamic rendering), but the Vulkan driver reported no graphics "
            "devices.\n";
    }
    else if (r.best < 0)
    {
        s = "Slipstream needs a graphics card and driver with Vulkan 1.3 support (or Vulkan 1.2 with the VK_KHR_dynamic_rendering extension). None of the graphics devices on "
            "this computer meets that yet.\n";
    }
    else
    {
        s = "Slipstream could not start the graphics device";
        s += r.failure.empty() ? std::string(".\n") : " (" + r.failure + ").\n";
    }

    bool intel = false, nvidia = false, amd = false;
    if (!r.gpus.empty())
    {
        s += "\nGraphics found:\n";
        for (const VkGpuRequirements& g : r.gpus)
        {
            char buf[384];
            std::snprintf(buf, sizeof(buf), "- %s: driver %s, Vulkan %u.%u\n", g.name.c_str(), g.driverText.c_str(),
                          VK_VERSION_MAJOR(g.apiVersion), VK_VERSION_MINOR(g.apiVersion));
            s += buf;
            if (!g.qualifies())
            {
                s += "    Missing: ";
                const size_t show = g.missing.size() > 4 ? 3 : g.missing.size();
                for (size_t i = 0; i < show; ++i) s += (i ? ", " : "") + g.missing[i];
                if (show < g.missing.size())
                    s += ", and " + std::to_string(g.missing.size() - show) + " more (see the log)";
                s += "\n";
            }
            intel |= g.vendorID == kVendorIntel;
            nvidia |= g.vendorID == kVendorNVIDIA;
            amd |= g.vendorID == kVendorAMD;
        }
    }
    if (!intel && !nvidia && !amd) intel = nvidia = amd = true;   // nothing identified: advise all

    {   // #675: name the driver as the fix when an extension / feature is what is absent.
        bool driverFeature = false;
        for (const VkGpuRequirements& g : r.gpus)
            for (const std::string& m : g.missing)
                if (m.rfind("extension ", 0) == 0 || m.rfind("feature ", 0) == 0) driverFeature = true;
        if (driverFeature)
            s += "\nThe items listed as Missing are features your graphics driver does not provide. "
                 "Updating the graphics driver usually adds them.\n";
    }
    s += "\nWhat to do:\n";
#if defined(_WIN32)
    if (intel)
        s += "- Intel: update the graphics driver with the Intel Driver & Support Assistant, or "
             "install Intel's graphics driver for your processor generation. Laptops may need the driver from the "
             "laptop maker's support site.\n";
    if (nvidia) s += "- NVIDIA: install the latest driver from nvidia.com/drivers.\n";
    if (amd) s += "- AMD: install the latest driver from amd.com/support.\n";
#elif defined(__APPLE__)
    s += "- macOS is not supported by Slipstream.\n";
#else
    if (intel || amd)
        s += "- Update Mesa (your distribution's graphics drivers) to a current version.\n";
    if (nvidia) s += "- NVIDIA: install a current NVIDIA driver from your distribution or nvidia.com/drivers.\n";
#endif
    s += "- If your computer has a second (dedicated) graphics card, make sure it is enabled and its driver is "
         "installed.\n";
    s += "\nYou do NOT need to install the Vulkan SDK to run Slipstream (it is for programmers); the graphics driver "
         "from your graphics card maker is what is needed.\n";
    s += "\nMore help: ";
    s += kVkRequirementsHelpURL;
    return s;
}

// ---- #950 driver-setup check ------------------------------------------------------------

const char* vkDriverCheckTitle()
{
    return "Slipstream: your graphics driver may not be set up correctly";
}

namespace
{
bool bigThreeVendor(uint32_t v)
{
    return v == kVendorNVIDIA || v == kVendorAMD || v == kVendorIntel;
}

// A GPU the OS reports that looks like a real discrete card (not software, big vendor, >= 1 GiB
// of its own memory; integrated GPUs report a small carve-out).
bool osDiscrete(const VkOsAdapter& a)
{
    return !a.software && bigThreeVendor(a.vendorID) && a.vramBytes >= (1ull << 30);
}

std::string driverLinkFor(uint32_t vendorID)
{
    switch (vendorID)
    {
    case kVendorNVIDIA: return "NVIDIA: download the latest driver from nvidia.com/drivers (choose your graphics card, then install it).";
    case kVendorAMD:    return "AMD: download the latest driver from amd.com/support.";
    case kVendorIntel:  return "Intel: download the latest graphics driver from intel.com/support.";
    default: return "";
    }
}
} // namespace

VkDriverCheck vkClassifyDriverSetup(const VkRequirementsReport& r, const std::vector<VkOsAdapter>& osAdapters, bool userChose)
{
    VkDriverCheck out;
    if (r.best < 0 || r.best >= (int)r.gpus.size()) return out;
    const VkGpuRequirements& c = r.gpus[r.best];
    char kb[320];
    std::snprintf(kb, sizeof(kb), "%s|0x%08X|0x%04X", c.name.c_str(), c.driverVersion, c.vendorID);
    out.key = kb;

    // Real-driver devices the Vulkan loader reports, and which vendors have one.
    const VkGpuRequirements* realDiscrete = nullptr;
    bool vendorHasRealVk[3] = {false, false, false};   // NVIDIA, AMD, Intel
    for (const VkGpuRequirements& g : r.gpus)
    {
        if (g.cls != VK_DEVICE_REAL) continue;
        if (g.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && (!realDiscrete || (g.qualifies() && !realDiscrete->qualifies())))
            realDiscrete = &g;
        if (g.vendorID == kVendorNVIDIA) vendorHasRealVk[0] = true;
        if (g.vendorID == kVendorAMD)    vendorHasRealVk[1] = true;
        if (g.vendorID == kVendorIntel)  vendorHasRealVk[2] = true;
    }
    auto hasRealVk = [&](uint32_t v)
    {
        return v == kVendorNVIDIA ? vendorHasRealVk[0] : v == kVendorAMD ? vendorHasRealVk[1] : v == kVendorIntel ? vendorHasRealVk[2] : true;
    };
    const VkOsAdapter* missing = nullptr;   // a discrete OS adapter with no Vulkan driver
    const VkOsAdapter* osDisc = nullptr;
    for (const VkOsAdapter& a : osAdapters)
    {
        if (!osDiscrete(a)) continue;
        if (!osDisc) osDisc = &a;
        if (!hasRealVk(a.vendorID) && !missing) missing = &a;
    }

    uint32_t realVendorID = 0;
    if (c.cls != VK_DEVICE_REAL)
    {
        out.issue = (c.cls == VK_DEVICE_SOFTWARE) ? VkSetupIssue::Software : VkSetupIssue::Translation;
        if (realDiscrete)   { out.realGpuName = realDiscrete->name; realVendorID = realDiscrete->vendorID; }
        else if (missing)   { out.realGpuName = missing->name;      realVendorID = missing->vendorID; }
        else if (osDisc)    { out.realGpuName = osDisc->name;       realVendorID = osDisc->vendorID; }
    }
    else if (missing)
    {
        out.issue = VkSetupIssue::DriverNotInstalled;
        out.realGpuName = missing->name;
        realVendorID = missing->vendorID;
    }
    else if (!userChose && c.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && realDiscrete)
    {
        out.issue = realDiscrete->qualifies() ? VkSetupIssue::IntegratedWhileDiscrete : VkSetupIssue::DiscreteUnusable;
        out.realGpuName = realDiscrete->name;
        realVendorID = realDiscrete->vendorID;
    }
    if (realVendorID && bigThreeVendor(realVendorID)) out.realVendor = vkVendorName(realVendorID);

    // Log text: classification + every device (hardware info only).
    {
        std::string l = "Vulkan driver check: issue=";
        l += vkSetupIssueName(out.issue);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "; chosen '%s' (%s GPU, class %s, driverID %u, vendor 0x%04X %s, driver %s, raw 0x%08X)",
                      c.name.c_str(), vkDeviceTypeName(c.type), vkDeviceClassName((VkDeviceClass)c.cls), c.driverID,
                      c.vendorID, vkVendorName(c.vendorID), c.driverText.c_str(), c.driverVersion);
        l += buf;
        if (userChose) l += "; GPU chosen explicitly in the graphics card setting";
        for (const VkGpuRequirements& g : r.gpus)
        {
            std::snprintf(buf, sizeof(buf), "; vulkan device '%s' (%s, class %s, driverID %u, %s)", g.name.c_str(),
                          vkDeviceTypeName(g.type), vkDeviceClassName((VkDeviceClass)g.cls), g.driverID,
                          g.qualifies() ? "meets requirements" : "does NOT meet requirements");
            l += buf;
        }
        if (osAdapters.empty()) l += "; OS adapter list: none / not available";
        for (const VkOsAdapter& a : osAdapters)
        {
            std::snprintf(buf, sizeof(buf), "; OS adapter '%s' (vendor 0x%04X %s, %llu MiB%s%s)", a.name.c_str(), a.vendorID,
                          vkVendorName(a.vendorID), (unsigned long long)(a.vramBytes >> 20), a.software ? ", software" : "",
                          osDiscrete(a) ? ", discrete" : "");
            l += buf;
        }
        if (!out.realGpuName.empty()) l += "; real GPU: '" + out.realGpuName + "'";
        out.logText = l;
    }
    if (out.issue == VkSetupIssue::None) return out;

    // Plain-language dialog body.
    std::string s;
    switch (out.issue)
    {
    case VkSetupIssue::Translation:
        s = "Slipstream is drawing with \"" + c.name + "\", which is not your computer's real graphics driver. "
            "It is a compatibility layer that borrows another graphics system (for example Microsoft's \"Direct3D 12\" translation), "
            "and it often shows the world wrongly -- for example every texture white or missing -- and runs slowly.\n";
        if (!out.realGpuName.empty())
            s += "Your computer has " + out.realGpuName + ", but Slipstream could not use its own driver.\n";
        break;
    case VkSetupIssue::Software:
        s = "Slipstream is drawing with \"" + c.name + "\", a software renderer that uses your processor instead of a graphics card. "
            "It is very slow and may show the world wrongly.\n";
        if (!out.realGpuName.empty())
            s += "Your computer has " + out.realGpuName + ", but Slipstream could not use its own driver.\n";
        break;
    case VkSetupIssue::DriverNotInstalled:
        s = "Your computer has " + out.realGpuName + ", but the graphics driver that lets Slipstream use it does not seem to be installed "
            "(or is not available to Slipstream). Slipstream is running on \"" + c.name + "\" instead, which is slower and may show the world wrongly.\n";
        break;
    case VkSetupIssue::IntegratedWhileDiscrete:
        s = "Slipstream is running on the basic graphics built into your processor (\"" + c.name + "\") although your computer also has " +
            out.realGpuName + ". That is usually much slower.\n";
        break;
    case VkSetupIssue::DiscreteUnusable:
        s = "Your computer has " + out.realGpuName + ", but its graphics driver is too old for Slipstream, so Slipstream is running on the "
            "basic graphics built into your processor (\"" + c.name + "\") instead. That is usually much slower.\n";
        break;
    default: break;
    }

    s += "\nHow to fix it:\n";
    const std::string link = driverLinkFor(realVendorID);
    if (!link.empty())
        s += "- Install or update the graphics driver. " + link + " Then restart your computer.\n";
    else
        s += "- Install or update the graphics driver for your graphics card: NVIDIA: nvidia.com/drivers; AMD: amd.com/support; "
             "Intel: intel.com/support. Then restart your computer.\n";
#if defined(_WIN32)
    s += "- On a laptop: open Windows Settings > System > Display > Graphics, add Slipstream to the list, choose Options, "
         "and set it to \"High performance\".\n";
#else
    s += "- On a laptop with two graphics chips: start Slipstream on the dedicated one (for example with the "
         "\"Launch using Dedicated Graphics Card\" menu entry, or prime-run / DRI_PRIME=1).\n";
#endif
    s += "\nYou do NOT need to install the Vulkan SDK to run Slipstream -- it is for programmers, and installing it does not "
         "fix this.\n";
    s += "\nMore help: ";
    s += kVkRequirementsHelpURL;
    out.userText = s;
    return out;
}
