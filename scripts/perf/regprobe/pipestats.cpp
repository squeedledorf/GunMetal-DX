// Offline register-count probe: creates a compute pipeline from a SPIR-V file on the local GPU with
// VK_KHR_pipeline_executable_properties and prints its statistics. Descriptor layouts are reflected
// from the module (set / binding decorations + variable types).
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <fstream>
#include <string>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, (int)r_); exit(1); } } while (0)

struct Ty { uint32_t op = 0; std::vector<uint32_t> w; };

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: pipestats file.spv...\n"); return 1; }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);

    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &pe};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
    VkPhysicalDeviceShaderAtomicFloatFeaturesEXT af{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, &as};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &af};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    as.accelerationStructureCaptureReplay = VK_FALSE;
    as.accelerationStructureHostCommands = VK_FALSE;
    const char* exts[] = {"VK_KHR_ray_query", "VK_KHR_acceleration_structure", "VK_KHR_deferred_host_operations",
                          "VK_KHR_pipeline_executable_properties", "VK_EXT_shader_atomic_float"};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 5;
    dci.ppEnabledExtensionNames = exts;
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, nullptr, &dev));
    auto getExe = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR");
    auto getStat = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR");

    for (int a = 1; a < argc; ++a)
    {
        std::ifstream f(argv[a], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
        std::vector<uint32_t> code(bytes.size() / 4);
        memcpy(code.data(), bytes.data(), code.size() * 4);
        std::map<uint32_t, Ty> types;
        std::map<uint32_t, uint32_t> consts, dset, dbind;
        std::map<uint32_t, bool> block, bufblock;
        std::vector<std::pair<uint32_t, uint32_t>> vars;   // id, ptr type
        for (size_t i = 5; i < code.size();)
        {
            const uint32_t wc = code[i] >> 16, op = code[i] & 0xFFFF;
            const uint32_t* w = &code[i];
            if (op == 71) { if (w[2] == 34) dset[w[1]] = w[3]; else if (w[2] == 33) dbind[w[1]] = w[3]; else if (w[2] == 2) block[w[1]] = true; else if (w[2] == 3) bufblock[w[1]] = true; }
            else if (op == 43) consts[w[2]] = w[3];
            else if (op == 59) vars.push_back({w[2], w[1]});
            else if ((op >= 19 && op <= 39) || op == 5341 || op == 4472)
            { Ty t; t.op = op; t.w.assign(w + 1, w + wc); types[w[1]] = t; }
            i += wc;
        }
        std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> sets;
        for (auto& v : vars)
        {
            if (!dset.count(v.first)) continue;
            const Ty& p = types[v.second];   // OpTypePointer: id, sc, type
            const uint32_t sc = p.w[1];
            uint32_t t = p.w[2];
            uint32_t count = 1;
            while (types[t].op == 28 || types[t].op == 29)
            {
                if (types[t].op == 28) count *= consts[types[t].w[2]];
                else count = 4096;
                t = types[t].w[1];
            }
            const Ty& b = types[t];
            VkDescriptorType dt;
            if (sc == 2) dt = bufblock[t] ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else if (sc == 12) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (b.op == 25) { const uint32_t dim = b.w[2], sampled = b.w[6];
                if (dim == 5) dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                else dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; }
            else if (b.op == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            else if (b.op == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if (b.op == 5341 || b.op == 4472) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            else { fprintf(stderr, "unknown descriptor type op %u\n", b.op); continue; }
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = dbind[v.first];
            lb.descriptorType = dt;
            lb.descriptorCount = count;
            lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            sets[dset[v.first]][lb.binding] = lb;
        }
        uint32_t maxSet = 0;
        for (auto& s : sets) maxSet = std::max(maxSet, s.first + 1);
        std::vector<VkDescriptorSetLayout> layouts(maxSet);
        for (uint32_t s = 0; s < maxSet; ++s)
        {
            std::vector<VkDescriptorSetLayoutBinding> bs;
            std::vector<VkDescriptorBindingFlags> fl;
            for (auto& kv : sets[s]) { bs.push_back(kv.second); fl.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT); }
            VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
            bf.bindingCount = (uint32_t)fl.size();
            bf.pBindingFlags = fl.data();
            VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
            lci.bindingCount = (uint32_t)bs.size();
            lci.pBindings = bs.data();
            CK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layouts[s]));
        }
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = maxSet;
        plci.pSetLayouts = layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        VkPipelineLayout pl;
        CK(vkCreatePipelineLayout(dev, &plci, nullptr, &pl));
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule sm;
        CK(vkCreateShaderModule(dev, &smci, nullptr, &sm));
        // PS_SPEC="id=value,id=value": 32-bit specialization constants, as the viewer sets them (e.g. "1=0": the
        // lighting kernels' kDebugViews off -- llcleanprojstochvk.cpp pdnLitSpec / llcleanvisqueuevk.cpp vqLitSpec)
        std::vector<VkSpecializationMapEntry> specMap;
        std::vector<uint32_t> specData;
        if (const char* sp = getenv("PS_SPEC"))
        {
            std::string s(sp);
            size_t p = 0;
            while (p < s.size())
            {
                size_t c = s.find(',', p);
                if (c == std::string::npos) c = s.size();
                const std::string kv = s.substr(p, c - p);
                const size_t eq = kv.find('=');
                if (eq != std::string::npos)
                {
                    const uint32_t id = (uint32_t)strtoul(kv.substr(0, eq).c_str(), nullptr, 0);
                    const uint32_t val = (uint32_t)strtoul(kv.substr(eq + 1).c_str(), nullptr, 0);
                    specMap.push_back({id, (uint32_t)(specData.size() * 4u), 4u});
                    specData.push_back(val);
                }
                p = c + 1;
            }
        }
        VkSpecializationInfo si{(uint32_t)specMap.size(), specMap.data(), specData.size() * 4u, specData.data()};
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | (getenv("PS_IR") ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0);
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main",
                      specMap.empty() ? nullptr : &si};
        cpci.layout = pl;
        VkPipeline pipe;
        CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe));
        VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        pi.pipeline = pipe;
        uint32_t ne = 0;
        getExe(dev, &pi, &ne, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> ex(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        getExe(dev, &pi, &ne, ex.data());
        for (uint32_t e = 0; e < ne; ++e)
        {
            VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = pipe;
            ei.executableIndex = e;
            uint32_t ns = 0;
            getStat(dev, &ei, &ns, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            getStat(dev, &ei, &ns, st.data());
            printf("%s:", argv[a]);
            for (auto& s : st)
            {
                if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR) printf(" %s=%llu;", s.name, (unsigned long long)s.value.u64);
                else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR) printf(" %s=%lld;", s.name, (long long)s.value.i64);
                else if (s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR) printf(" %s=%g;", s.name, s.value.f64);
            }
            printf("\n");
            if (getenv("PS_IR"))
            {
                auto getIR = (PFN_vkGetPipelineExecutableInternalRepresentationsKHR)vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableInternalRepresentationsKHR");
                uint32_t nr = 0;
                getIR(dev, &ei, &nr, nullptr);
                std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
                getIR(dev, &ei, &nr, ir.data());
                std::vector<std::vector<char>> bufs(nr);
                for (uint32_t k = 0; k < nr; ++k) { bufs[k].resize(ir[k].dataSize + 1); ir[k].pData = bufs[k].data(); }
                getIR(dev, &ei, &nr, ir.data());
                for (uint32_t k = 0; k < nr; ++k)
                {
                    printf("IR %u: %s (%s) text=%d size=%zu\n", k, ir[k].name, ir[k].description, (int)ir[k].isText, ir[k].dataSize);
                    std::string fn = std::string(argv[a]) + ".ir" + std::to_string(k) + ".txt";
                    FILE* o = fopen(fn.c_str(), "wb");
                    if (o) { fwrite(bufs[k].data(), 1, ir[k].dataSize, o); fclose(o); }
                }
            }
        }
        vkDestroyPipeline(dev, pipe, nullptr);
        vkDestroyShaderModule(dev, sm, nullptr);
        vkDestroyPipelineLayout(dev, pl, nullptr);
        for (auto l : layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return 0;
}
