// vkclean — native Vulkan clean-renderer prototype entry point.
//
//   vkclean --out <file.png> [--size N] [--shaders <dir>]
//
// Initializes Vulkan 1.3 (discrete GPU) with the Khronos validation layer +
// sync-validation (shader-access heuristic ON), runs the clean-pipeline
// GPU-driven prototype against the REAL clean data layouts, prints the
// per-bucket GPU-vs-CPU cull comparison, writes the golden PNG, and exits 0
// only if: all six bucket counts match, the PNG has real coverage, and
// validation is clean.
#include "clean_scene.h"
#include "vk_device.h"
#include "vk_offscreen.h"

#include <cstdlib>
#include <cstring>
#include <string>

static const char* kUsage =
    "usage: vkclean --out <file.png> [--size N] [--shaders <dir>]\n";

static const char* kBucketName[6] = {
    "opaque", "mask", "alpha", "r_opaque", "r_mask", "r_alpha"};

int main(int argc, char** argv) {
    std::string out;
    std::string shaderDir = "shaders";
    uint32_t size = 768;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s", kUsage); std::exit(2); }
            return argv[++i];
        };
        if (a == "--out") out = next();
        else if (a == "--size") size = (uint32_t)std::atoi(next().c_str());
        else if (a == "--shaders") shaderDir = next();
        else { std::fprintf(stderr, "unknown arg '%s'\n%s", a.c_str(), kUsage); return 2; }
    }
    if (out.empty()) { std::fprintf(stderr, "%s", kUsage); return 2; }

    VkDeviceContext ctx;
    ctx.init(/*enableValidation=*/true);

    OffscreenTarget target;
    target.init(ctx, size, size, /*withDepth=*/true);

    CleanResult r{};
    bool ok = sceneClean(ctx, target, shaderDir, out, r);

    VK_LOG("---- vkclean SUMMARY ----");
    VK_LOG("instances uploaded: %u (real 192-byte LLCleanInstanceStore::Instance layout)",
           r.instanceCount);
    VK_LOG("per-bucket emitted draws (GPU compute cull vs CPU reference):");
    for (int b = 0; b < 6; ++b)
        VK_LOG("  %-9s : GPU %4u   CPU %4u   %s",
               kBucketName[b], r.gpu_bucket[b], r.cpu_bucket[b],
               r.gpu_bucket[b] == r.cpu_bucket[b] ? "match" : "MISMATCH");
    VK_LOG("cull match (all buckets): %s", r.cullMatch ? "YES" : "NO");
    VK_LOG("PNG has geometry coverage: %s", r.nonEmpty ? "YES" : "NO");
    VK_LOG("validation: %u error(s), %u warning(s)",
           r.validationErrors, r.validationWarnings);

    target.destroy(ctx);
    ctx.shutdown();

    if (!ok) {
        std::fprintf(stderr, "[vkclean] FAIL: cull mismatch, empty image, or PNG write failed\n");
        return 1;
    }
    if (r.validationErrors > 0 || r.validationWarnings > 0) {
        std::fprintf(stderr, "[vkclean] FAIL: validation not clean\n");
        return 1;
    }
    VK_LOG("vkclean OK");
    return 0;
}
