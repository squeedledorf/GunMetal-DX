#include "vk_device.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// VkResult -> string (small subset that we actually might hit)
// ---------------------------------------------------------------------------
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
        default: return "VK_ERROR_<other>";
    }
}

// ---------------------------------------------------------------------------
// Debug messenger
// ---------------------------------------------------------------------------
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
        return; // too noisy for a spike
    }
    std::fprintf(stderr, "[validation:%s] %s\n", tag, msg);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
void VkDeviceContext::createInstance(bool enableValidation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vktest";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "alchemy-vk-spike";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> exts;
    if (enableValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    if (enableValidation) ci.pNext = &layerSettings;

    VK_CHECK(vkCreateInstance(&ci, nullptr, &mInstance));

    if (enableValidation) {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        if (create) {
            VkDebugUtilsMessengerCreateInfoEXT m{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            m.messageSeverity =
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
}

// ---------------------------------------------------------------------------
// Physical device selection (prefer discrete)
// ---------------------------------------------------------------------------
void VkDeviceContext::pickPhysicalDevice() {
    uint32_t n = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(mInstance, &n, nullptr));
    if (n == 0) { std::fprintf(stderr, "no Vulkan devices\n"); std::abort(); }
    std::vector<VkPhysicalDevice> devs(n);
    VK_CHECK(vkEnumeratePhysicalDevices(mInstance, &n, devs.data()));

    VkPhysicalDevice best = VK_NULL_HANDLE;
    VkPhysicalDevice fallback = devs[0];
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            best = d; break;
        }
    }
    mPhysical = best ? best : fallback;

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(mPhysical, &p);
    VK_LOG("GPU: %s  (Vulkan %u.%u.%u)", p.deviceName,
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
           VK_VERSION_PATCH(p.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(mPhysical, &mMemProps);

    // Find a queue family with graphics (implies compute+transfer on NVIDIA).
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysical, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { mQueueFamily = i; break; }
    }
}

// ---------------------------------------------------------------------------
// Logical device — enable the modern feature set the port would rely on.
// ---------------------------------------------------------------------------
void VkDeviceContext::createLogicalDevice() {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = mQueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    // Chain of feature structs. We enable the pieces a GPU-driven path needs so
    // the RHI foundation is proven capable, even though stages 1-4 use a subset.
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

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
    f12.pNext = &f13;

    VkPhysicalDeviceFeatures2 f2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.multiDrawIndirect = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.pNext = &f12;

    std::vector<const char*> devExts = {
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    };

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    VK_CHECK(vkCreateDevice(mPhysical, &dci, nullptr, &mDevice));
    vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = mQueueFamily;
    VK_CHECK(vkCreateCommandPool(mDevice, &pci, nullptr, &mCommandPool));
}

bool VkDeviceContext::init(bool enableValidation) {
    mValidation = enableValidation;
    createInstance(enableValidation);
    pickPhysicalDevice();
    createLogicalDevice();
    return true;
}

void VkDeviceContext::shutdown() {
    if (mCommandPool) vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
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

GpuBuffer VkDeviceContext::createBuffer(VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags props,
                                        bool deviceAddress) {
    GpuBuffer b{};
    b.size = size;
    if (deviceAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(mDevice, &ci, nullptr, &b.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(mDevice, b.buffer, &req);

    VkMemoryAllocateFlagsInfo flags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (deviceAddress) ai.pNext = &flags;
    VK_CHECK(vkAllocateMemory(mDevice, &ai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(mDevice, b.buffer, b.memory, 0));

    if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        VK_CHECK(vkMapMemory(mDevice, b.memory, 0, size, 0, &b.mapped));
    }
    if (deviceAddress) {
        VkBufferDeviceAddressInfo di{
            VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        di.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(mDevice, &di);
    }
    return b;
}

void VkDeviceContext::destroyBuffer(GpuBuffer& b) {
    if (b.mapped) { vkUnmapMemory(mDevice, b.memory); b.mapped = nullptr; }
    if (b.buffer) vkDestroyBuffer(mDevice, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(mDevice, b.memory, nullptr);
    b = GpuBuffer{};
}

GpuImage VkDeviceContext::createImage2D(uint32_t w, uint32_t h,
                                        VkFormat format,
                                        VkImageUsageFlags usage,
                                        VkImageAspectFlags aspect) {
    GpuImage img{};
    img.format = format;
    img.extent = {w, h};

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
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(mDevice, &ci, nullptr, &img.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(mDevice, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(mDevice, &ai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(mDevice, img.image, img.memory, 0));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(mDevice, &vi, nullptr, &img.view));
    return img;
}

void VkDeviceContext::destroyImage(GpuImage& img) {
    if (img.view) vkDestroyImageView(mDevice, img.view, nullptr);
    if (img.image) vkDestroyImage(mDevice, img.image, nullptr);
    if (img.memory) vkFreeMemory(mDevice, img.memory, nullptr);
    img = GpuImage{};
}

void VkDeviceContext::submitImmediate(
    const std::function<void(VkCommandBuffer)>& record) {
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
    record(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

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
                                                     bool deviceAddress) {
    GpuBuffer staging = createBuffer(
        size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(staging.mapped, data, (size_t)size);

    GpuBuffer dst = createBuffer(
        size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, deviceAddress);

    submitImmediate([&](VkCommandBuffer cmd) {
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, staging.buffer, dst.buffer, 1, &region);
    });
    destroyBuffer(staging);
    return dst;
}
