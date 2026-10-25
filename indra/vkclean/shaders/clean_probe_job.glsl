// vkclean -- GPU-DRIVEN REFLECTION PROBES (#478): the capture JOB records.
//
// clean_probe_gpu.comp (phase RANK_D) chooses this frame's captures on the GPU and writes
// one record per job: [0] the SCHEDULED job (one cube face per frame, LL doProbeUpdate),
// [1] the REALTIME job (the nearest resident dynamic probe, all faces every frame). The
// CPU records a fixed command stream for both every frame and never learns which probe
// they capture: the cameras reach the consumers through
//   * the draw-view / cull-params ring entries the capture reserved, patched from these
//     records by clean_probe_patch.comp (the world draws and the cull read the ring);
//   * this buffer by device address (push constant `uvec2`) in the passes that take
//     their camera as a push constant: the probe alpha sort, the avatar cull, the
//     ray-traced capture and the filter (its target slot);
//   * indirect dispatch arguments (filter mips / irradiance, the trace): an empty job
//     dispatches zero workgroups.
// Byte layout = LLCleanRendererVK::PrbJobGPU (std430, 720 B; static_assert'd).
#ifndef CLEAN_PROBE_JOB_GLSL
#define CLEAN_PROBE_JOB_GLSL
// (Include it before any declaration: with CLEAN_PROBE_JOB_REF it enables extensions.)
#ifdef CLEAN_PROBE_JOB_REF
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#endif

#define PJ_SCHED      0u      // job index: the scheduled capture
#define PJ_REALTIME   1u      // job index: the realtime capture
#define PJ_COUNT      2u

#define PJ_SKY        1u      // info.z: sky only (the default probe)
#define PJ_DYNAMIC    2u      // info.z: avatars in (LL getIsDynamic, detail >= 1)
#define PJ_TRACED     4u      // info.z: ray-traced faces (detail 3, sphere inside the TLAS radius)
#define PJ_NO_ALPHA   8u      // info.z: no blended alpha in this capture (#834 RenderVKProbeAlpha: far scheduled
                              // probes; the water stays) -- the probe alpha sort keeps the water only, the avatar
                              // cull empties the rigged alpha lists, the ray-traced capture drops its blend layer

#define PJ_FILTER_ARGS 16u    // filt[]: [0, mips) radiance mips, [PJ_IRR_ARG] irradiance
#define PJ_IRR_ARG     15u

struct PrbJob {
    vec4  origin;      // xyz capture origin (agent space), w near clip
    vec4  clip;        // x far clip, y radius (influence), z, w unused
    uvec4 info;        // x valid (1), y cube slot, z PJ_* flags, w candidate index
    uvec4 info2;       // x key lo, y key hi, z light list (0 scheduled, 1 realtime), w unused
    mat4  vp[6];       // world -> clip of each face (LLCleanRendererVK probeFaceMatrix)
    uvec4 filt[PJ_FILTER_ARGS];   // VkDispatchIndirectCommand per filter dispatch (.w unused)
    uvec4 trace;       // VkDispatchIndirectCommand of the ray-traced capture
};                     // 720 B

#ifdef CLEAN_PROBE_JOB_REF
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PrbJobRef { PrbJob jobs[]; };

// The job record `j` of the job buffer at device address `addr` (a push-constant uvec2).
PrbJob prbJob(uvec2 addr, uint j)
{
    PrbJobRef r = PrbJobRef(addr);
    return r.jobs[j];
}
bool prbJobValid(uvec2 addr, uint j)
{
    PrbJobRef r = PrbJobRef(addr);
    return r.jobs[j].info.x != 0u;
}
#endif

#endif
