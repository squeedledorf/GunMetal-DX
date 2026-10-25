/**
 * @file slfxc.cpp
 * @brief Slipstream ReShade FX smoke-test compiler (#543, FX P0). Developer tool only:
 *        not part of the viewer binary.
 *
 * Compiles one or more ReShade FX (.fx) files through the viewer's own compile front
 * end (vkgfx/llfxcompile.cpp: the runtime's predefined macros, compatibility prelude and
 * codegen settings), dumps the effect module (techniques / passes / textures / samplers / storages / uniforms + offsets / spec
 * constants), writes one SPIR-V module per entry point, validates each with
 * `spirv-val --target-env vulkan1.3` (when on PATH) and, with --vk, creates a
 * VkShaderModule for each on a real device (validation layer on when installed).
 *
 * usage: slfxc [-I dir]... [-D name=value]... [--width W --height H] [--out dir]
 *              [--vk [--time-pipes] [--no-opt] [--robust]] [--quiet] [--summary file.tsv] file.fx...
 *
 * --seed-cache FILE creates the pipeline cache from the viewer's vk_pipeline_cache.bin and
 * builds every pipeline through it (as the viewer does).
 * --linger MS (with --vk) watches the process's threads for MS after the last compile and
 * prints which ones still burn CPU (driver background compiles), then times vkDestroyDevice.
 * --exit-test exit|fast (with --vk) builds the first effect's pipelines on a second thread
 * and leaves the process from main 1 s later via exit(0) or _exit(0), the device alive
 * (the viewer's quit): does a thread inside the driver's compiler hold the exit up?
 * --time-pipes prints the wall time of every vkCreate*Pipelines call (per pass); --no-opt
 * builds with VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT; --robust enables
 * VK_EXT_pipeline_robustness on the device and requests robustBufferAccess2 /
 * robustImageAccess2 per pipeline, as the viewer does. Run cold driver timings with
 * __GL_SHADER_DISK_CACHE=0 (NVIDIA) so the driver's own disk cache does not answer.
 *
 * Exit code: 0 = every file compiled and validated, 1 = at least one failed.
 *
 * Copyright (c) 2026, Slipstream Viewer contributors. LGPL-2.1 (as the viewer).
 */

#include "llfxcompile.h"

#if defined(SLFXC_WITH_VULKAN)
#include <vulkan/vulkan.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <thread>
#if defined(__linux__)
#include <unistd.h>
#endif
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct Options
{
    std::vector<std::string> includes;
    std::vector<std::pair<std::string, std::string>> defines;
    std::string width = "1920", height = "1080";
    std::string outDir;
    std::string summary;
    bool vk = false;
    bool quiet = false;
    bool timePipes = false;   // --time-pipes: print vkCreate*Pipelines wall time per pass
    bool noOpt = false;       // --no-opt: VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT
    bool robust = false;      // --robust: VK_EXT_pipeline_robustness (robustBufferAccess2 / robustImageAccess2)
    int lingerMs = 0;
    std::string exitTest;     // --exit-test exit|fast: leave the process while a thread compiles
    std::string seedCache;    // --seed-cache FILE: create a VkPipelineCache from the viewer's vk_pipeline_cache.bin
    bool useCache = false;    // pipelines go through that cache (as the viewer's vkSharedPipelineCache())         // --linger MS: after the last compile, report per-thread CPU (driver threads) for MS
};
Options gOpt;

const char* typeName(const reshadefx::type& t)
{
    static thread_local char buf[32];
    const char* base = t.is_boolean() ? "bool" : t.is_integral() ? (t.is_signed() ? "int" : "uint")
                     : t.is_floating_point() ? "float" : t.is_texture() ? "texture" : t.is_sampler() ? "sampler"
                     : t.is_storage() ? "storage" : t.is_struct() ? "struct" : "?";
    if (t.is_numeric() && (t.rows > 1 || t.cols > 1))
    {
        if (t.cols > 1) std::snprintf(buf, sizeof(buf), "%s%ux%u", base, (unsigned)t.rows, (unsigned)t.cols);
        else std::snprintf(buf, sizeof(buf), "%s%u", base, (unsigned)t.rows);
        return buf;
    }
    return base;
}

std::string annotationsText(const std::vector<reshadefx::annotation>& anns)
{
    std::string s;
    for (const auto& a : anns)
    {
        if (!s.empty()) s += ", ";
        s += a.name + "=";
        if (a.type.base == reshadefx::type::t_string) s += "\"" + a.value.string_data + "\"";
        else if (a.type.is_floating_point()) s += std::to_string(a.value.as_float[0]);
        else if (a.type.is_integral() || a.type.is_boolean()) s += std::to_string(a.value.as_int[0]);
    }
    return s;
}

#if defined(SLFXC_WITH_VULKAN)
struct VkCtx
{
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    uint32_t validationMessages = 0;
};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* d, void* user)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        ++static_cast<VkCtx*>(user)->validationMessages;
        std::fprintf(stderr, "  [validation] %s\n", d->pMessage);
    }
    return VK_FALSE;
}

bool vkInit(VkCtx& c)
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    bool val = false;
    for (const auto& l : layers) val = val || std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "slfxc"; ai.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT dm{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dm.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dm.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    dm.pfnUserCallback = debugCb; dm.pUserData = &c;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    if (val) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer; ci.enabledExtensionCount = 1;
               ci.ppEnabledExtensionNames = &ext; ci.pNext = &dm; }
    if (vkCreateInstance(&ci, nullptr, &c.inst) != VK_SUCCESS) return false;
    uint32_t pn = 0;
    vkEnumeratePhysicalDevices(c.inst, &pn, nullptr);
    if (!pn) return false;
    std::vector<VkPhysicalDevice> pds(pn);
    vkEnumeratePhysicalDevices(c.inst, &pn, pds.data());
    VkPhysicalDevice pd = pds[0];
    for (VkPhysicalDevice p : pds)
    {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(p, &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = p; break; }
    }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    std::printf("vk device: %s (validation layer %s)\n", pp.deviceName, val ? "on" : "not installed");
    float prio = 1.f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = 0; q.queueCount = 1; q.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDevicePipelineRobustnessFeaturesEXT fr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT r2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceImageRobustnessFeatures ir{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES};
    const char* devExts[2] = {VK_EXT_PIPELINE_ROBUSTNESS_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    if (gOpt.robust)
    {
        fr.pipelineRobustness = VK_TRUE;
        r2.robustBufferAccess2 = VK_TRUE; r2.robustImageAccess2 = VK_TRUE;
        f13.robustImageAccess = VK_TRUE;
        f13.pNext = &fr; fr.pNext = &r2;
        dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = devExts;
    }
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
    if (vkCreateDevice(pd, &dci, nullptr, &c.dev) != VK_SUCCESS) return false;
    if (gOpt.useCache)
    {
        // The viewer's file: a 192-byte SLVKPCH header (vk_pipecache.cpp), then the driver payload.
        std::ifstream in(gOpt.seedCache, std::ios::binary);
        std::vector<char> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint32_t hdr = 0;
        if (blob.size() > 16 && std::memcmp(blob.data(), "SLVKPCH", 7) == 0) std::memcpy(&hdr, blob.data() + 12, 4);
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (hdr && hdr < blob.size()) { ci.initialDataSize = blob.size() - hdr; ci.pInitialData = blob.data() + hdr; }
        const auto t0 = std::chrono::steady_clock::now();
        const VkResult r = vkCreatePipelineCache(c.dev, &ci, nullptr, &c.cache);
        std::printf("pipeline cache: seeded %zu bytes, result %d, %.1f ms\n", (size_t)ci.initialDataSize, (int)r,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    return true;
}

// Pipelines the way llfxruntimevk.cpp builds them (set 0 dynamic UBO, set 1 combined
// samplers, set 2 storage images at the codegen's bindings; dynamic rendering). Validation
// messages count as failures. Kept deliberately close to buildPipelines() in the runtime.
VkFormat texFormat(reshadefx::texture_format f)
{
    using tf = reshadefx::texture_format;
    switch (f)
    {
    case tf::r8: return VK_FORMAT_R8_UNORM;   case tf::r16f: return VK_FORMAT_R16_SFLOAT;
    case tf::r16: return VK_FORMAT_R16_UNORM; case tf::r32f: return VK_FORMAT_R32_SFLOAT;
    case tf::r32u: return VK_FORMAT_R32_UINT; case tf::r32i: return VK_FORMAT_R32_SINT;
    case tf::rg8: return VK_FORMAT_R8G8_UNORM; case tf::rg16f: return VK_FORMAT_R16G16_SFLOAT;
    case tf::rg16: return VK_FORMAT_R16G16_UNORM; case tf::rg32f: return VK_FORMAT_R32G32_SFLOAT;
    case tf::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT; case tf::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
    case tf::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT; case tf::rgba32u: return VK_FORMAT_R32G32B32A32_UINT;
    case tf::rgba32i: return VK_FORMAT_R32G32B32A32_SINT; case tf::rgb10a2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case tf::rg11b10f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

bool buildAllPipelines(VkCtx& vk, const slfx::CompileResult& res, std::string& why)
{
    const reshadefx::effect_module& m = res.module;
    VkDevice dev = vk.dev;
    std::vector<VkShaderModule> mods;
    std::vector<VkDescriptorSetLayout> dsls;
    std::vector<VkPipelineLayout> pls;
    std::vector<VkPipeline> pipes;
    bool ok = true;
    auto modFor = [&](const std::string& ep) -> VkShaderModule {
        auto it = res.spirv.find(ep);
        if (it == res.spirv.end()) return VK_NULL_HANDLE;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = it->second.size() * 4; ci.pCode = it->second.data();
        VkShaderModule sm = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &sm);
        mods.push_back(sm);
        return sm;
    };
    VkDescriptorSetLayout dsl0 = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &b;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl0);
        dsls.push_back(dsl0);
    }
    const uint32_t before = vk.validationMessages;
    for (const auto& t : m.techniques)
        for (const auto& ps : t.passes)
        {
            std::vector<VkDescriptorSetLayoutBinding> b1, b2;
            for (const auto& sb : ps.sampler_bindings)
                b1.push_back({sb.entry_point_binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr});
            for (const auto& ub : ps.storage_bindings)
                b2.push_back({ub.entry_point_binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_ALL, nullptr});
            VkDescriptorSetLayout d1 = VK_NULL_HANDLE, d2 = VK_NULL_HANDLE;
            VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)b1.size(); li.pBindings = b1.data();
            vkCreateDescriptorSetLayout(dev, &li, nullptr, &d1);
            li.bindingCount = (uint32_t)b2.size(); li.pBindings = b2.data();
            vkCreateDescriptorSetLayout(dev, &li, nullptr, &d2);
            dsls.push_back(d1); dsls.push_back(d2);
            const VkDescriptorSetLayout sets[3] = {dsl0, d1, d2};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 3; pli.pSetLayouts = sets;
            VkPipelineLayout pl = VK_NULL_HANDLE;
            vkCreatePipelineLayout(dev, &pli, nullptr, &pl);
            pls.push_back(pl);
            VkPipeline pipe = VK_NULL_HANDLE;
            VkResult r;
            double pipeMs = 0.0;
            VkPipelineRobustnessCreateInfoEXT rob{VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT};
            rob.storageBuffers = rob.uniformBuffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_2_EXT;
            rob.vertexInputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DEVICE_DEFAULT_EXT;
            rob.images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_ROBUST_IMAGE_ACCESS_2_EXT;
            if (!ps.cs_entry_point.empty())
            {
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = modFor(ps.cs_entry_point);
                ci.stage.pName = ps.cs_entry_point.c_str(); ci.layout = pl;
                if (gOpt.noOpt) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                if (gOpt.robust) ci.pNext = &rob;
                const auto pt0 = std::chrono::steady_clock::now();
                r = vkCreateComputePipelines(dev, vk.cache, 1, &ci, nullptr, &pipe);
                pipeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pt0).count();
            }
            else
            {
                VkFormat fmts[8] = {};
                uint32_t n = 0;
                if (ps.render_target_names[0].empty())
                {
                    fmts[0] = ps.srgb_write_enable ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
                    n = 1;
                }
                else
                    for (; n < 8 && !ps.render_target_names[n].empty(); ++n)
                        for (const auto& tx : m.textures)
                            if (tx.unique_name == ps.render_target_names[n])
                            {
                                fmts[n] = texFormat(tx.format);
                                if (ps.srgb_write_enable && fmts[n] == VK_FORMAT_R8G8B8A8_UNORM) fmts[n] = VK_FORMAT_R8G8B8A8_SRGB;
                            }
                VkPipelineShaderStageCreateInfo st[2]{};
                uint32_t nst = 0;
                st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                st[nst].stage = VK_SHADER_STAGE_VERTEX_BIT; st[nst].module = modFor(ps.vs_entry_point);
                st[nst].pName = ps.vs_entry_point.c_str();
                ++nst;
                if (!ps.ps_entry_point.empty())
                {
                    st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    st[nst].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[nst].module = modFor(ps.ps_entry_point);
                    st[nst].pName = ps.ps_entry_point.c_str();
                    ++nst;
                }
                VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                using pt = reshadefx::primitive_topology;
                ia.topology = ps.topology == pt::point_list ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                            : ps.topology == pt::line_list ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                            : ps.topology == pt::line_strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                            : ps.topology == pt::triangle_strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                            : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                vp.viewportCount = 1; vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.lineWidth = 1.f;
                VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
                ds.stencilTestEnable = ps.stencil_enable;
                VkPipelineColorBlendAttachmentState cb[8]{};
                for (uint32_t k = 0; k < n; ++k)
                {
                    cb[k].blendEnable = ps.blend_enable[k];
                    cb[k].srcColorBlendFactor = cb[k].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                    cb[k].colorWriteMask = ps.render_target_write_mask[k] & 0xF;
                }
                VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
                cbs.attachmentCount = n; cbs.pAttachments = cb;
                const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
                dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
                VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
                ri.colorAttachmentCount = n; ri.pColorAttachmentFormats = fmts;
                ri.stencilAttachmentFormat = ps.stencil_enable ? VK_FORMAT_S8_UINT : VK_FORMAT_UNDEFINED;
                VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                gi.pNext = &ri; gi.stageCount = nst; gi.pStages = st;
                gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
                gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds;
                gi.pColorBlendState = &cbs; gi.pDynamicState = &dy; gi.layout = pl;
                if (gOpt.noOpt) gi.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                if (gOpt.robust) ri.pNext = &rob;
                const auto pt0 = std::chrono::steady_clock::now();
                r = vkCreateGraphicsPipelines(dev, vk.cache, 1, &gi, nullptr, &pipe);
                pipeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pt0).count();
            }
            if (gOpt.timePipes)
                std::printf("  pipe\t%s\t%s\t%.1f ms\n", t.name.c_str(), ps.name.c_str(), pipeMs);
            std::fflush(stdout);
            if (r != VK_SUCCESS) { ok = false; why = "pipeline for pass " + ps.name + ": result " + std::to_string((int)r); }
            if (pipe) pipes.push_back(pipe);
        }
    if (vk.validationMessages != before) { ok = false; if (why.empty()) why = "validation messages while building pipelines"; }
    for (VkPipeline p : pipes) vkDestroyPipeline(dev, p, nullptr);
    for (VkPipelineLayout l : pls) vkDestroyPipelineLayout(dev, l, nullptr);
    for (VkDescriptorSetLayout l : dsls) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    for (VkShaderModule sm : mods) if (sm) vkDestroyShaderModule(dev, sm, nullptr);
    return ok;
}
#endif

// --linger: after the compiles returned, does the driver keep compiling on its own threads
// (NVIDIA: "[vkps] Update" / "[vkcf] Analysis" / "[vkrt] Analysis")? Prints every thread
// that used CPU in each 250 ms window (Linux /proc only).
void lingerThreads(int ms)
{
#if defined(__linux__)
    std::map<std::string, long> prev;
    auto sample = [](std::map<std::string, long>& out) {
        out.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator("/proc/self/task", ec))
        {
            std::ifstream c(e.path() / "comm"), st(e.path() / "stat");
            std::string comm; std::getline(c, comm);
            std::string line; std::getline(st, line);
            const size_t rp = line.rfind(')');
            if (rp == std::string::npos) continue;
            std::istringstream is(line.substr(rp + 2));
            std::string f; long ut = 0, stt = 0;
            for (int k = 3; k <= 15; ++k) { is >> f; if (k == 14) ut = std::atol(f.c_str()); if (k == 15) stt = std::atol(f.c_str()); }
            out[e.path().filename().string() + " " + comm] = ut + stt;
        }
    };
    sample(prev);
    for (int t = 0; t < ms; t += 250)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::map<std::string, long> now;
        sample(now);
        for (const auto& kv : now)
        {
            const long d = kv.second - (prev.count(kv.first) ? prev[kv.first] : 0);
            if (d > 0) std::printf("  linger t=%d ms thread [%s] +%ld ticks\n", t + 250, kv.first.c_str(), d);
        }
        prev.swap(now);
    }
#else
    (void)ms;
#endif
}

bool runSpirvVal(const std::string& path, std::string& err)
{
    static int have = -1;
    if (have < 0) have = std::system("spirv-val --version > /dev/null 2>&1") == 0 ? 1 : 0;
    if (!have) return true;
    const std::string log = path + ".val.txt";
    const std::string cmd = "spirv-val --target-env vulkan1.3 \"" + path + "\" > \"" + log + "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == 0) { fs::remove(log); return true; }
    std::ifstream in(log);
    std::stringstream ss; ss << in.rdbuf();
    err = ss.str();
    return false;
}
}

int main(int argc, char* argv[])
{
    Options& o = gOpt;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-I" && i + 1 < argc) o.includes.push_back(argv[++i]);
        else if (a == "-D" && i + 1 < argc)
        {
            std::string d = argv[++i];
            const size_t eq = d.find('=');
            o.defines.emplace_back(d.substr(0, eq), eq == std::string::npos ? "1" : d.substr(eq + 1));
        }
        else if (a == "--width" && i + 1 < argc) o.width = argv[++i];
        else if (a == "--height" && i + 1 < argc) o.height = argv[++i];
        else if (a == "--out" && i + 1 < argc) o.outDir = argv[++i];
        else if (a == "--summary" && i + 1 < argc) o.summary = argv[++i];
        else if (a == "--vk") o.vk = true;
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--time-pipes") o.timePipes = true;
        else if (a == "--no-opt") o.noOpt = true;
        else if (a == "--robust") o.robust = true;
        else if (a == "--linger" && i + 1 < argc) o.lingerMs = std::atoi(argv[++i]);
        else if (a == "--exit-test" && i + 1 < argc) o.exitTest = argv[++i];
        else if (a == "--seed-cache" && i + 1 < argc) { o.seedCache = argv[++i]; o.useCache = true; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else files.push_back(a);
    }
    if (files.empty())
    {
        std::fprintf(stderr, "usage: slfxc [-I dir]... [-D name=value]... [--width W --height H] [--out dir] "
                             "[--vk] [--quiet] [--summary file.tsv] file.fx...\n");
        return 2;
    }
#if defined(SLFXC_WITH_VULKAN)
    VkCtx vk;
    if (o.vk && !vkInit(vk)) { std::fprintf(stderr, "Vulkan init failed\n"); return 2; }
#else
    if (o.vk) { std::fprintf(stderr, "built without Vulkan (SLFXC_WITH_VULKAN)\n"); return 2; }
#endif
    std::ofstream summary;
    if (!o.summary.empty()) summary.open(o.summary);
    int failed = 0;
    for (const std::string& file : files)
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::string stage = "ok", why;
        uint32_t modules = 0, passes = 0;
        slfx::CompileRequest req;
        req.path = file;
        req.includeDirs = o.includes;
        req.defines = o.defines;
        req.width = (uint32_t)std::strtoul(o.width.c_str(), nullptr, 10);
        req.height = (uint32_t)std::strtoul(o.height.c_str(), nullptr, 10);
        req.vendorId = 0x10DE;
        slfx::CompileResult res;
        if (!slfx::compile(req, res))
        {
            stage = res.module.techniques.empty() && res.spirv.empty() ? "compile" : "assemble";
            why = res.errors;
        }
        else
        {
            const reshadefx::effect_module& m = res.module;
            if (!o.quiet)
            {
                std::printf("== %s\n", file.c_str());
                for (const auto& t : m.textures)
                    std::printf("  texture %s %ux%u mips %u fmt %d%s%s%s\n", t.unique_name.c_str(), t.width, t.height,
                                (unsigned)t.levels, (int)t.format, t.semantic.empty() ? "" : (" semantic " + t.semantic).c_str(),
                                t.render_target ? " RT" : "", t.storage_access ? " UAV" : "");
                for (const auto& s : m.samplers)
                    std::printf("  sampler %s -> %s filter %d addr %d/%d srgb %d\n", s.unique_name.c_str(),
                                s.texture_name.c_str(), (int)s.filter, (int)s.address_u, (int)s.address_v, (int)s.srgb);
                for (const auto& s : m.storages)
                    std::printf("  storage %s -> %s level %u\n", s.unique_name.c_str(), s.texture_name.c_str(), (unsigned)s.level);
                for (const auto& u : m.uniforms)
                    std::printf("  uniform %s %s off %u size %u {%s}\n", typeName(u.type), u.name.c_str(), u.offset, u.size,
                                annotationsText(u.annotations).c_str());
                for (const auto& u : m.spec_constants)
                    std::printf("  spec %s %s\n", typeName(u.type), u.name.c_str());
                std::printf("  uniform block %u bytes\n", m.total_uniform_size);
                for (const auto& t : m.techniques)
                {
                    std::printf("  technique %s {%s}\n", t.name.c_str(), annotationsText(t.annotations).c_str());
                    for (const auto& p : t.passes)
                        std::printf("    pass %s vs=%s ps=%s cs=%s rt0=%s verts=%u samplers=%zu storages=%zu\n",
                                    p.name.c_str(), p.vs_entry_point.c_str(), p.ps_entry_point.c_str(),
                                    p.cs_entry_point.c_str(), p.render_target_names[0].c_str(), p.num_vertices,
                                    p.sampler_bindings.size(), p.storage_bindings.size());
                }
            }
            for (const auto& t : m.techniques) passes += (uint32_t)t.passes.size();
            for (const auto& ep : m.entry_points)
            {
                const std::vector<uint32_t>& words = res.spirv.at(ep.first);
                const std::string spirv(reinterpret_cast<const char*>(words.data()), words.size() * 4);
                ++modules;
                fs::path out = o.outDir.empty() ? fs::temp_directory_path() / "slfxc" : fs::path(o.outDir);
                fs::create_directories(out);
                out /= fs::path(file).stem().string() + "." + ep.first + ".spv";
                std::ofstream(out, std::ios::binary).write(spirv.data(), (std::streamsize)spirv.size());
                std::string verr;
                if (!runSpirvVal(out.string(), verr))
                {
                    stage = "spirv-val"; why = ep.first + ": " + verr;
                    break;
                }
#if defined(SLFXC_WITH_VULKAN)
                if (o.vk)
                {
                    const uint32_t before = vk.validationMessages;
                    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    ci.codeSize = spirv.size(); ci.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
                    VkShaderModule sm = VK_NULL_HANDLE;
                    const VkResult r = vkCreateShaderModule(vk.dev, &ci, nullptr, &sm);
                    if (sm) vkDestroyShaderModule(vk.dev, sm, nullptr);
                    if (r != VK_SUCCESS || vk.validationMessages != before)
                    {
                        stage = "vkCreateShaderModule"; why = ep.first + ": result " + std::to_string((int)r);
                        break;
                    }
                }
#endif
            }
#if defined(SLFXC_WITH_VULKAN)
            if (o.vk && stage == "ok" && !o.exitTest.empty())
            {
                // --exit-test: the shutdown race of the FX worker. Build the pipelines on a
                // second thread, and leave the process from main while it is (likely) still
                // inside the driver: "exit" = exit(0) (static destructors + library
                // finalizers, the device left alive as the viewer does), "fast" = _exit(0).
                static slfx::CompileResult keep;
                keep = res;
                std::thread([&vk] { std::string pw; buildAllPipelines(vk, keep, pw); std::printf("worker: pipelines returned\n"); std::fflush(stdout); }).detach();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                std::printf("main: calling %s(0) with the worker inside vkCreateGraphicsPipelines\n",
                            o.exitTest == "fast" ? "_exit" : "exit");
                std::fflush(stdout);
                if (o.exitTest == "fast") _exit(0);
                std::exit(0);
            }
            if (o.vk && stage == "ok")
            {
                std::string pw;
                if (!buildAllPipelines(vk, res, pw)) { stage = "pipelines"; why = pw; }
            }
#endif
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (stage != "ok") ++failed;
        // First error line only in the summary.
        std::string first = why.substr(0, why.find('\n'));
        std::printf("%s\t%s\t%u modules\t%u passes\t%.1f ms\t%s\n", stage == "ok" ? "PASS" : "FAIL", file.c_str(),
                    modules, passes, ms, stage == "ok" ? "" : (stage + ": " + first).c_str());
        if (summary) summary << (stage == "ok" ? "PASS" : "FAIL") << '\t' << file << '\t' << stage << '\t' << first << '\n';
    }
#if defined(SLFXC_WITH_VULKAN)
    if (o.vk && o.lingerMs > 0) lingerThreads(o.lingerMs);
    {
        const auto d0 = std::chrono::steady_clock::now();
        if (vk.cache) vkDestroyPipelineCache(vk.dev, vk.cache, nullptr);
        if (vk.dev) vkDestroyDevice(vk.dev, nullptr);
        const auto d1 = std::chrono::steady_clock::now();
        if (vk.inst) vkDestroyInstance(vk.inst, nullptr);
        const auto d2 = std::chrono::steady_clock::now();
        if (o.lingerMs > 0)
            std::printf("vkDestroyDevice %.1f ms, vkDestroyInstance %.1f ms\n",
                        std::chrono::duration<double, std::milli>(d1 - d0).count(),
                        std::chrono::duration<double, std::milli>(d2 - d1).count());
    }
#endif
    return failed ? 1 : 0;
}
