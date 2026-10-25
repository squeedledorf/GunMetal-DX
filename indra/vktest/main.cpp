// vktest — Vulkan de-risking spike entry point.
//
//   vktest --scene <name> --out <file.png> [--size N]
//
// Initializes Vulkan (prefers the discrete GPU) with the Khronos validation
// layer, renders the named scene to an offscreen NxN RGBA8 target, writes the
// PNG, reports validation output, and exits 0 on success.
#include "scenes.h"
#include "scenes_gpu.h"
#include "vk_device.h"
#include "vk_offscreen.h"

#include <cstdlib>
#include <cstring>
#include <string>

static const char* kUsage =
    "usage: vktest --scene "
    "<clear|triangle|texquad|litcube|gpucubes|gpucubes_stress> "
    "--out <file.png> [--size N] [--shaders <dir>]\n";

int main(int argc, char** argv) {
    std::string scene, out;
    std::string shaderDir = "shaders";
    uint32_t size = 512;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s", kUsage); std::exit(2); }
            return argv[++i];
        };
        if (a == "--scene") scene = next();
        else if (a == "--out") out = next();
        else if (a == "--size") size = (uint32_t)std::atoi(next().c_str());
        else if (a == "--shaders") shaderDir = next();
        else { std::fprintf(stderr, "unknown arg '%s'\n%s", a.c_str(), kUsage);
               return 2; }
    }
    if (scene.empty() || out.empty()) { std::fprintf(stderr, "%s", kUsage); return 2; }

    VkDeviceContext ctx;
    ctx.init(/*enableValidation=*/true);

    OffscreenTarget target;
    const bool needsDepth =
        (scene == "litcube" || scene == "gpucubes" || scene == "gpucubes_stress");
    target.init(ctx, size, size, needsDepth);

    // gpucubes_stress is an experiment: phase-1 sync-validation warnings are the
    // EXPECTED result, so it must NOT be judged by the global exit-on-validation
    // rule. It is judged only by its own GpuStressResult (phase-2 must be clean
    // + correct draw count).
    if (scene == "gpucubes_stress") {
        GpuStressResult r;
        bool ran = sceneGpuCubesStress(ctx, target, shaderDir, r);
        bool wrote = ran && target.readbackToPng(ctx, out);

        VK_LOG("---- gpucubes_stress SUMMARY ----");
        VK_LOG("Phase 1 (UNSYNCHRONIZED): %u error(s), %u warning(s) "
               "-> hazard %s by sync-validation",
               r.hazardErrors, r.hazardWarnings,
               (r.hazardErrors + r.hazardWarnings) > 0 ? "DETECTED" : "NOT detected");
        VK_LOG("Phase 2 (SYNCHRONIZED):   %u error(s), %u warning(s) "
               "-> %s; final draw count %u (expected %u)",
               r.correctErrors, r.correctWarnings,
               (r.correctErrors + r.correctWarnings) == 0 ? "CLEAN" : "STILL FLAGGED",
               r.finalDrawCount, r.expectedVisible);

        target.destroy(ctx);
        ctx.shutdown();

        if (!ran || !wrote) {
            std::fprintf(stderr, "[vktest] gpucubes_stress: experiment failed to run\n");
            return 1;
        }
        // Pass criteria: phase 2 must be validation-clean AND render correctly.
        if (r.correctErrors + r.correctWarnings != 0 ||
            r.finalDrawCount != r.expectedVisible) {
            std::fprintf(stderr, "[vktest] gpucubes_stress: corrected phase "
                         "not clean/correct\n");
            return 1;
        }
        VK_LOG("scene '%s' OK", scene.c_str());
        return 0;
    }

    bool ok = (scene == "gpucubes")
                  ? sceneGpuCubes(ctx, target, shaderDir)
                  : renderScene(scene, ctx, target, shaderDir);
    if (ok) ok = target.readbackToPng(ctx, out);

    // Report validation tally (capture before shutdown resets the context).
    uint32_t vErrors = ctx.validationErrorCount();
    uint32_t vWarns = ctx.validationWarnCount();
    VK_LOG("validation: %u error(s), %u warning(s)", vErrors, vWarns);

    target.destroy(ctx);
    ctx.shutdown();

    if (!ok) return 1;
    if (vErrors > 0) {
        std::fprintf(stderr, "[vktest] FAIL: validation errors present\n");
        return 1;
    }
    VK_LOG("scene '%s' OK", scene.c_str());
    return 0;
}
