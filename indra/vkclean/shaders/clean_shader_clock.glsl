// vkclean — SHADER CLOCK STAGE TIMING (RenderVKShaderClockLighting; VK_KHR_shader_clock).
//
// Where the deferred lighting compute's time goes, per stage, on the GPU itself: each
// CLK_MARK(stage) reads the subgroup clock (clock2x32ARB, low word: per-SM shader cycles)
// and charges the cycles since this invocation's previous mark to `stage`. Every interval
// lands in exactly one stage (nested work splits itself: a mark before and after a call
// inside a loop charges the loop body around it to the outer stage), so the stages sum to
// the invocation's total. clean_deferred_light.comp reduces the per-invocation sums once at
// the end (subgroupAdd + one atomic per stage per subgroup) into set 2 binding 7, read back
// by LLCleanRendererVK::readShaderClockStats.
//
// Compiled in ONLY with -DCLEAN_SHADER_CLOCK (the *ClockSpv variants, regen_world_spv.py):
// every other build -- the forward lit FS, the production lighting kernels -- gets empty
// CLK_MARK / CLK_SURF macros and no clock capability in its SPIR-V.
//
// APPROXIMATE ATTRIBUTION: the compilers (glslc -O, the driver backend) may schedule
// arithmetic across a clock read, and a texture / image load is charged where its result is
// first used (issue is cheap, the wait is not), so the boundary between neighbouring stages
// is soft. Shares across many pixels are meaningful; a single stage's cycles are not exact.

#ifndef CLEAN_SHADER_CLOCK_GLSL
#define CLEAN_SHADER_CLOCK_GLSL

// Stage slots (= LLCleanRendererVK kClkStageNames; one list for every kernel, unused 0).
#define CLK_S_TAIL         0u    // after the last mark (early outs, the epilogue)
#define CLK_S_CLUSTER_SCAN 1u    // tiled kernels: the lit-guard cluster scan (lgClusterScan; the light grid build under it)
#define CLK_S_TILE_DEPTH   2u    // tiled kernels / the light grid build: G2 + depth load, position, tile z min / max (+ bins) + barriers
#define CLK_S_TILE_LIGHTS  3u    // tiled kernels / the light grid build: the tile light test + barrier
#define CLK_S_GBUF         4u    // G-buffer load / decode (+ position; the half / sharp classification)
#define CLK_S_SUNVIS       5u    // sun / moon visibility (cascades / RT mask; combine: the visibility read)
#define CLK_S_PROJ         6u    // projector visibility (atlas / RT mask; combine: the slot read)
#define CLK_S_ATMOS        7u    // atmosLightTerms
#define CLK_S_PROBE        8u    // reflection probe walk + sky fallback (combine: the reflection read)
#define CLK_S_SSR          9u    // the SSR march
#define CLK_S_MIRROR       10u   // the planar mirror tap (+ the legacy env / clamp tail)
#define CLK_S_SHADE        11u   // AO, BRDF, sun / ambient / env combine (the material branches)
#define CLK_S_LOCAL        12u   // local lights (tile list walk), without their projector visibility
#define CLK_S_HAZE         13u   // haze / water fog + distance fade (surfacePost)
#define CLK_S_STORE        14u   // output stores (HDR + glow, the split intermediates, the light grid)
#define CLK_S_GRID         15u   // tiled kernels under the light grid (DEFER_FLAG_LIGHT_GRID): G2 + depth
                                 // load, position, the tile's grid mask -> shared + the one barrier
#define CLK_S_FROXEL       16u   // lit kernels' local-light walk: finding the light set -- the pixel's froxel
                                 // cell (RenderVKLightCulling 2: index, first mask word, subgroup OR) or
                                 // the shared tile mask's first word (0 / 1)
#define CLK_STAGES         17u
#define CLK_SLOT_INV       17u   // invocations that ran the kernel (in bounds)
#define CLK_SLOT_SURF      18u   // ... of which had a G-buffer surface
#define CLK_SLOTS          19u
#define CLK_KERNELS        11u   // = CLEAN_DEFER_PASS 0..10 (6: the light grid build; 7-9: the #827 feature split;
                                 // 10: shadow + tile classify, #831)
#define CLK_BUCKETS        16u   // counter copies (spread the atomics; the host sums them)

#ifdef CLEAN_SHADER_CLOCK
uint g_clk_prev = 0u;
bool g_clk_inv = false;    // CLK_INV: this invocation is in bounds (counted in CLK_SLOT_INV)
bool g_clk_surf = false;   // CLK_SURF: ... and has a G-buffer surface (CLK_SLOT_SURF)
uint g_clk_TAIL = 0u, g_clk_CLUSTER_SCAN = 0u, g_clk_TILE_DEPTH = 0u, g_clk_TILE_LIGHTS = 0u, g_clk_GBUF = 0u,
     g_clk_SUNVIS = 0u, g_clk_PROJ = 0u, g_clk_ATMOS = 0u, g_clk_PROBE = 0u, g_clk_SSR = 0u, g_clk_MIRROR = 0u,
     g_clk_SHADE = 0u, g_clk_LOCAL = 0u, g_clk_HAZE = 0u, g_clk_STORE = 0u, g_clk_GRID = 0u, g_clk_FROXEL = 0u;
uint clkNow() { return clock2x32ARB().x; }   // (wrapping uint deltas: any stage < 2^32 cycles)
// Separate scalars, not an array: every accumulator stays a register.
#define CLK_MARK(st) { const uint clk_t_ = clkNow(); g_clk_##st += clk_t_ - g_clk_prev; g_clk_prev = clk_t_; }
#define CLK_INV() g_clk_inv = true
#define CLK_SURF() g_clk_surf = true
#else
#define CLK_INV()
#define CLK_MARK(st)
#define CLK_SURF()
#endif

#endif   // CLEAN_SHADER_CLOCK_GLSL
