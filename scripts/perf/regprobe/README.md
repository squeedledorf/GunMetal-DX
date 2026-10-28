# regprobe: shader register counts without a viewer build

Compiles `clean_deferred_light.comp` variants with the build's flags (`glslc -O`), creates each
compute pipeline on the local GPU and prints the driver's statistics via
VK_KHR_pipeline_executable_properties (NVIDIA: register count, local memory; numbers match the
viewer's `VKPipeStats` lines within about +-2, the difference being spec constants left at defaults).
A register question takes about a minute instead of a viewer rebuild (#1039 register pass).

    g++ -std=c++17 -O2 scripts/perf/regprobe/pipestats.cpp -lvulkan -o /tmp/regprobe/pipestats
    REGPROBE_OUT=/tmp/regprobe python3 scripts/perf/regprobe/measure.py RtSplitMatVq2 GiSplitHitMatVq2
    REGPROBE_OUT=/tmp/regprobe python3 scripts/perf/regprobe/measure.py RtSplitMatVq2 -DSOME_EXPERIMENT :: RtSplitMat
    python3 scripts/perf/regprobe/sweep.py      # compile-check every listed variant
    PS_SPEC=1=0 REGPROBE_OUT=/tmp/regprobe python3 scripts/perf/regprobe/measure.py shadow   # pass 15 + its queue kernels

`PS_SPEC="id=value,..."` sets 32-bit specialization constants the way the viewer does: the lighting kernels
are created with `kDebugViews` (constant_id 1) off (`pdnLitSpec` / `vqLitSpec` / `giLitSpec`), so `1=0` gives
the viewer's variant; without it the debug views stay compiled in (a few registers more on some kernels).

Occupancy tiers (NVIDIA, 64K registers per SM): warps/SM = 65536 / (regs x 32): 128 -> 16, 168 -> 12, 216 -> 9.
