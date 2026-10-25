// texenc_roundtrip.cpp -- offline round-trip check of the GPU block encoder
// (indra/vkclean/shaders/clean_texenc.comp, RenderVKTextureCompression #263; #840).
//
// Runs the real compute shader on the local Vulkan device over synthetic RGBA images
// (opaque card with dark text / hair detail, near-opaque J2C-like alpha noise, an
// anti-aliased rounded cutout, RGBA noise), decodes the blocks with an independent
// reference decoder (bcdec, iOrange, MIT / Unlicense -- downloaded by run.sh, not part of
// the viewer) and compares ALPHA per texel. Fails (exit 1) when a texel whose source
// alpha is on the kept side of an alpha-mask cutoff decodes below it (or the reverse) for
// an encode that was told that cutoff (EncPush.flags bits [8,16)). Without a cutoff it
// only reports (opaque source texels decoding more than 16 levels low: plain least-squares
// error, which has no side contract).
//
// Build + run: scripts/texenc_test/run.sh   (needs glslc, g++, a Vulkan device)
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed: %d\n", #x, (int)r_); std::exit(2); } } while (0)

namespace
{
struct EncPush { uint32_t lod, w, h, bx, by, out_word, mode, flags, stat_rec; };
enum { ENC_BC3 = 2, ENC_BC7_FAST = 5, ENC_BC7_HQ = 6 };

VkInstance gInst; VkPhysicalDevice gPhys; VkDevice gDev; VkQueue gQ; uint32_t gQF = 0;
VkCommandPool gPool;

uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(gPhys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    std::fprintf(stderr, "no memory type\n"); std::exit(2);
}
struct Buf { VkBuffer b; VkDeviceMemory m; void* p; };
Buf makeBuf(VkDeviceSize n, VkBufferUsageFlags u)
{
    Buf r{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = n; bi.usage = u;
    VKC(vkCreateBuffer(gDev, &bi, nullptr, &r.b));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(gDev, r.b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(gDev, &ai, nullptr, &r.m));
    VKC(vkBindBufferMemory(gDev, r.b, r.m, 0));
    VKC(vkMapMemory(gDev, r.m, 0, VK_WHOLE_SIZE, 0, &r.p));
    return r;
}
void freeBuf(Buf& b) { vkDestroyBuffer(gDev, b.b, nullptr); vkFreeMemory(gDev, b.m, nullptr); }

void initVk()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo = &app;
    VKC(vkCreateInstance(&ii, nullptr, &gInst));
    uint32_t n = 0; vkEnumeratePhysicalDevices(gInst, &n, nullptr);
    if (!n) { std::fprintf(stderr, "no Vulkan device\n"); std::exit(2); }
    std::vector<VkPhysicalDevice> pd(n); vkEnumeratePhysicalDevices(gInst, &n, pd.data());
    gPhys = pd[0];
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(gPhys, &qn, qp.data());
    for (uint32_t i = 0; i < qn; ++i) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { gQF = i; break; }
    float pr = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = gQF; qi.queueCount = 1; qi.pQueuePriorities = &pr;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    VKC(vkCreateDevice(gPhys, &di, nullptr, &gDev));
    vkGetDeviceQueue(gDev, gQF, 0, &gQ);
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex = gQF;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKC(vkCreateCommandPool(gDev, &ci, nullptr, &gPool));
}

// Encodes RGBA8 `img` (w x h, multiples of 4) with `mode`; returns the blocks.
std::vector<uint32_t> encode(VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSetLayout dsl,
                             const std::vector<uint8_t>& img, uint32_t w, uint32_t h, uint32_t mode, uint32_t cut8)
{
    // source image
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM; ic.extent = {w, h, 1};
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage im; VKC(vkCreateImage(gDev, &ic, nullptr, &im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(gDev, im, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imem; VKC(vkAllocateMemory(gDev, &ai, nullptr, &imem)); VKC(vkBindImageMemory(gDev, im, imem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ic.format; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; VKC(vkCreateImageView(gDev, &vi, nullptr, &view));
    Buf stg = makeBuf(img.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(stg.p, img.data(), img.size());
    const uint32_t bx = w / 4, by = h / 4, words = (mode == 1u || mode == 3u) ? 2u : 4u;
    Buf out = makeBuf((VkDeviceSize)bx * by * words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buf st = makeBuf(64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(st.p, 0, 64);

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    VkDescriptorPool dp; VKC(vkCreateDescriptorPool(gDev, &dpi, nullptr, &dp));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
    VkDescriptorSet ds; VKC(vkAllocateDescriptorSets(gDev, &dai, &ds));
    VkDescriptorImageInfo dii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo b1{out.b, 0, VK_WHOLE_SIZE}, b2{st.b, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[3]{};
    for (int i = 0; i < 3; ++i) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = ds; wr[i].dstBinding = i; wr[i].descriptorCount = 1; }
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[0].pImageInfo = &dii;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &b1;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[2].pBufferInfo = &b2;
    vkUpdateDescriptorSets(gDev, 3, wr, 0, nullptr);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = gPool; cai.commandBufferCount = 1;
    VkCommandBuffer cmd; VKC(vkAllocateCommandBuffers(gDev, &cai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VKC(vkBeginCommandBuffer(cmd, &bi));
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = im; b.subresourceRange = vi.subresourceRange; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{}; cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; cp.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, stg.b, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, nullptr);
    EncPush p{0, w, h, bx, by, 0, mode, 1u | (cut8 << 8), 0};
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cmd, (bx * by + 63) / 64, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VKC(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VKC(vkQueueSubmit(gQ, 1, &si, VK_NULL_HANDLE)); VKC(vkQueueWaitIdle(gQ));
    std::vector<uint32_t> r((size_t)bx * by * words);
    std::memcpy(r.data(), out.p, r.size() * 4);
    vkFreeCommandBuffers(gDev, gPool, 1, &cmd);
    vkDestroyDescriptorPool(gDev, dp, nullptr);
    freeBuf(stg); freeBuf(out); freeBuf(st);
    vkDestroyImageView(gDev, view, nullptr); vkDestroyImage(gDev, im, nullptr); vkFreeMemory(gDev, imem, nullptr);
    return r;
}

std::vector<uint8_t> decode(const std::vector<uint32_t>& blk, uint32_t w, uint32_t h, uint32_t mode)
{
    std::vector<uint8_t> o((size_t)w * h * 4);
    const uint32_t bx = w / 4;
    for (uint32_t y = 0; y < h / 4; ++y)
        for (uint32_t x = 0; x < bx; ++x)
        {
            const uint32_t* s = blk.data() + (size_t)(y * bx + x) * 4;
            uint8_t* d = o.data() + ((size_t)y * 4 * w + x * 4) * 4;
            if (mode == ENC_BC3) bcdec_bc3(s, d, (int)w * 4);
            else bcdec_bc7(s, d, (int)w * 4);
        }
    return o;
}
int bc7Mode(const uint32_t* s) { for (int m = 0; m < 8; ++m) if (s[0] & (1u << m)) return m; return 8; }

// ---- synthetic images ----
using Img = std::vector<uint8_t>;
void put(Img& im, uint32_t w, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{ uint8_t* p = &im[((size_t)y * w + x) * 4]; p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
// opaque card: pale background, dark "text" strokes, hair-like curves, saturated patches
Img card(uint32_t w, uint32_t h, std::mt19937& rng, int alpha_noise)
{
    Img im((size_t)w * h * 4);
    std::uniform_int_distribution<int> u(0, 255);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            put(im, w, x, y, (uint8_t)(220 + (x * 13 + y * 7) % 30), (uint8_t)(200 + (x * 5) % 40), (uint8_t)(180 + (y * 3) % 60));
    for (int s = 0; s < 400; ++s)   // dark strokes (text)
    {
        const int x0 = u(rng) % w, y0 = u(rng) % h, len = 3 + u(rng) % 12, horiz = u(rng) & 1;
        const uint8_t c = (uint8_t)(u(rng) % 40);
        for (int k = 0; k < len; ++k) { const int x = horiz ? std::min<int>(x0 + k, w - 1) : x0, y = horiz ? y0 : std::min<int>(y0 + k, h - 1); put(im, w, x, y, c, c, (uint8_t)(c + 10)); }
    }
    for (int s = 0; s < 60; ++s)    // hair: thin dark-brown curves
    {
        float x = (float)(u(rng) % w), y = (float)(u(rng) % h), a = (float)u(rng) * 0.0246f;
        for (int k = 0; k < 80; ++k)
        {
            x += std::cos(a); y += std::sin(a); a += ((float)u(rng) - 127.5f) * 0.002f;
            if (x < 0 || y < 0 || x >= w || y >= h) break;
            put(im, w, (int)x, (int)y, 60, 35, 20);
        }
    }
    for (int s = 0; s < 40; ++s)    // saturated patches
    {
        const int x0 = u(rng) % w, y0 = u(rng) % h; const uint8_t r = (uint8_t)u(rng), g = (uint8_t)u(rng), b = (uint8_t)u(rng);
        for (int y = y0; y < std::min<int>(y0 + 6, h); ++y) for (int x = x0; x < std::min<int>(x0 + 6, w); ++x) put(im, w, x, y, r, g, b);
    }
    if (alpha_noise)
        for (size_t p = 0; p < (size_t)w * h; ++p) im[p * 4 + 3] = (uint8_t)(255 - u(rng) % (alpha_noise + 1));
    return im;
}
// anti-aliased rounded corners (alpha 0 outside, soft 2-texel edge) over a card
void roundCorners(Img& im, uint32_t w, uint32_t h, float rad)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            const float cx = std::clamp((float)x + 0.5f, rad, (float)w - rad), cy = std::clamp((float)y + 0.5f, rad, (float)h - rad);
            const float d = std::hypot((float)x + 0.5f - cx, (float)y + 0.5f - cy) - rad;   // > 0 outside
            const float a = std::clamp(0.5f - d * 0.5f, 0.f, 1.f);
            uint8_t& pa = im[((size_t)y * w + x) * 4 + 3];
            pa = (uint8_t)std::lround(std::min((float)pa, a * 255.f));
            if (d > 2.f) { uint8_t* p = &im[((size_t)y * w + x) * 4]; p[0] = p[1] = p[2] = 255; }   // white RGB under alpha 0
        }
}
Img noise(uint32_t w, uint32_t h, std::mt19937& rng)
{
    Img im((size_t)w * h * 4);
    std::uniform_int_distribution<int> u(0, 255);
    for (auto& v : im) v = (uint8_t)u(rng);
    return im;
}
} // namespace

int main(int argc, char** argv)
{
    const char* spv_path = argc > 1 ? argv[1] : "clean_texenc.spv";
    std::ifstream f(spv_path, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(f)), {});
    if (code.empty() || code.size() % 4) { std::fprintf(stderr, "cannot read %s\n", spv_path); return 2; }
    initVk();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; smi.codeSize = code.size(); smi.pCode = (const uint32_t*)code.data();
    VkShaderModule sm; VKC(vkCreateShaderModule(gDev, &smi, nullptr, &sm));
    VkDescriptorSetLayoutBinding lb[3]{};
    for (int i = 0; i < 3; ++i) { lb[i].binding = i; lb[i].descriptorCount = 1; lb[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    lb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; lb[1].descriptorType = lb[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dli.bindingCount = 3; dli.pBindings = lb;
    VkDescriptorSetLayout dsl; VKC(vkCreateDescriptorSetLayout(gDev, &dli, nullptr, &dsl));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EncPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VkPipelineLayout pl; VKC(vkCreatePipelineLayout(gDev, &pli, nullptr, &pl));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    cpi.layout = pl;
    VkPipeline pipe; VKC(vkCreateComputePipelines(gDev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));

    const uint32_t W = 256, H = 256;
    std::mt19937 rng(840);
    struct Case { const char* name; Img img; };
    std::vector<Case> cases;
    cases.push_back({"card opaque", card(W, H, rng, 0)});
    { Img c = card(W, H, rng, 0); roundCorners(c, W, H, 24.f); cases.push_back({"card opaque + AA rounded cutout", c}); }
    cases.push_back({"card alpha noise 0..12", card(W, H, rng, 12)});
    { Img c = card(W, H, rng, 6); roundCorners(c, W, H, 24.f); cases.push_back({"card alpha noise 0..6 + cutout", c}); }
    cases.push_back({"rgba noise", noise(W, H, rng)});
    const uint32_t modes[] = {ENC_BC3, ENC_BC7_FAST, ENC_BC7_HQ};
    const char* mname[] = {"", "", "BC3", "", "", "BC7F", "BC7HQ"};
    const uint32_t cuts[] = {0, 128, 200, 250, 255};
    int failures = 0;
    for (const Case& c : cases)
        for (uint32_t m : modes)
            for (uint32_t cut : cuts)
            {
                const std::vector<uint32_t> blk = encode(pipe, pl, dsl, c.img, W, H, m, cut);
                const Img d = decode(blk, W, H, m);
                int flips = 0, opaque_lost = 0, max_err = 0;
                int mode_hist[9] = {}, mode_bad[9] = {};
                for (uint32_t by = 0; by < H / 4; ++by)
                    for (uint32_t bx = 0; bx < W / 4; ++bx)
                    {
                        const int bm = m == ENC_BC3 ? 8 : bc7Mode(&blk[(by * (W / 4) + bx) * 4]);
                        ++mode_hist[bm];
                        bool bad = false;
                        for (int i = 0; i < 16; ++i)
                        {
                            const size_t p = ((size_t)(by * 4 + i / 4) * W + bx * 4 + i % 4) * 4 + 3;
                            const int s = c.img[p], e = d[p];
                            max_err = std::max(max_err, std::abs(s - e));
                            if (cut)
                            {
                                // kept side: src > cut must stay > cut; src == cut stays >= cut; dropped stays < cut
                                const bool ok = s > (int)cut ? e > (int)cut : s == (int)cut ? e >= (int)cut : e < (int)cut;
                                if (!ok) { ++flips; bad = true; }
                            }
                            else if (s == 255 && e < 255 - 16) { ++opaque_lost; bad = true; }
                        }
                        if (bad) ++mode_bad[bm];
                    }
                const bool fail = flips != 0;   // (opaque lost: informational -- no cutoff, no contract)
                failures += fail ? 1 : 0;
                std::printf("%-34s %-5s cut %3u: max |da| %3d, cut flips %5d, opaque lost %5d  modes", c.name, mname[m], cut, max_err, flips, opaque_lost);
                for (int k = 0; k < 9; ++k) if (mode_hist[k]) std::printf(" %s%d:%d(bad %d)", k == 8 ? "bc3" : "m", k, mode_hist[k], mode_bad[k]);
                std::printf("%s\n", fail ? "  FAIL" : "");
            }
    std::printf("%s (%d failing case(s))\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
