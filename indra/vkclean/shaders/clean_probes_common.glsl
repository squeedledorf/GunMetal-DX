// vkclean — reflection-probe data shared by clean_probe_cluster.comp and clean_draw.frag.
// Byte layout matches LLCleanRendererVK::VKProbeGPU / VKProbeHeader (std430).

#define PROBE_CLUSTER_CAP 24u   // = LLCleanRendererVK::PROBE_CLUSTER_CAP (#249: 16 overflowed on probed builds)

struct Probe {
    vec4  sphere;   // world origin .xyz, influence radius .w (box: circumscribed)
    mat4  box;      // world -> unit cube [-1,1]^3 (box probes)
    vec4  params;   // x ambiance, y fade-in 0..1, z 1 = box, w 1 = manual / 0 = automatic
    ivec4 idx;      // x cube slot, y 1 = complete, z 1 = parallax sphere (manual),
                    // w class: 0 default, 1 manual, 2 terrain grid, 3 automatic (#478)
};                  // 112 B

struct ProbeHeader {
    vec4 info;      // x count, y max radiance lod, z sky probe ambiance, w enabled
};
