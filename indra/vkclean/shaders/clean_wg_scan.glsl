// vkclean -- workgroup-wide inclusive prefix sum of one uint per invocation, built on
// subgroup arithmetic: one barrier per call instead of the 2 x log2(N) of a shared-memory
// Hillis-Steele scan. Integer sums: the result is EXACTLY the Hillis-Steele scan's (same
// inclusive prefix per element, same total), so a stable compaction built on it emits the
// same entries at the same positions.
//
// ELEMENT ORDER: the scan runs in SUBGROUP-LINEAR order, wgLinearId() = gl_SubgroupID *
// gl_SubgroupSize + gl_SubgroupInvocationID -- the caller indexes its elements by that id
// (not gl_LocalInvocationID), so no invocation-to-subgroup mapping is assumed. That id is
// a bijection onto [0, WG) only when every subgroup is full.
//
// FULL SUBGROUPS ARE A PIPELINE PROPERTY, NOT SOMETHING THE SHADER CAN TEST (rigged-cull
// crash, TheSlowly report: this scan in 8ce310cc39 crashed a Windows user at a parcel,
// reliably; reverting the shader side stopped it). Without
// VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT the implementation may launch
// subgroups with inactive invocations and (SPIR-V 1.6) pick the subgroup size per dispatch,
// so gl_NumSubgroups * gl_SubgroupSize == WG proves nothing: a subgroup slot of s_wg_sg
// nobody wrote this call feeds a stale / uninitialised total into every later subgroup's
// prefix -> output indices past the list (GPU OOB write) and counts over the entries
// actually written (garbage indirect draws) -> device lost. So the caller takes this path
// only when the HOST says the pipeline was created with REQUIRE_FULL_SUBGROUPS on a device
// with computeFullSubgroups + compute subgroup arithmetic (and RenderVKRigCullSubgroupScan):
// the `allowed` push bit (LLCleanRendererVK::rigCullScanBits). wgScanUsable() stays as a
// workgroup-uniform sanity check on top.
//
// The including shader requires GL_KHR_shader_subgroup_basic + _arithmetic (as
// clean_cull.comp does) and calls wgInclusiveScan from workgroup-uniform control flow
// with every invocation of the workgroup active.
//
// The per-subgroup totals are double-buffered by call parity: the call after next writes
// the same half only after the next call's barrier, which every invocation reaches only
// after its reads of this call -- consecutive calls need no barrier of their own.
#ifndef CLEAN_WG_SCAN_GLSL
#define CLEAN_WG_SCAN_GLSL

#define WG_SCAN_MAX_SG 64u   // subgroups per workgroup (256 invocations at subgroup size >= 4)

shared uint s_wg_sg[2u * WG_SCAN_MAX_SG];
uint g_wg_par = 0u;

uint wgLinearId() { return gl_SubgroupID * gl_SubgroupSize + gl_SubgroupInvocationID; }

// `allowed`: the host's push bit (full subgroups guaranteed by the pipeline; see above).
// Then wgLinearId() covers [0, wg) once each; workgroup-uniform.
bool wgScanUsable(uint wg, bool allowed)
{
    return allowed && gl_NumSubgroups * gl_SubgroupSize == wg && gl_NumSubgroups <= WG_SCAN_MAX_SG;
}

// Inclusive prefix of v over the workgroup in wgLinearId() order; `total` = the sum over
// the whole workgroup (the same in every invocation).
uint wgInclusiveScan(uint v, out uint total)
{
    const uint incl = subgroupInclusiveAdd(v);
    const uint sum  = subgroupAdd(v);
    const uint half_ = g_wg_par * WG_SCAN_MAX_SG;
    g_wg_par ^= 1u;
    if (subgroupElect()) s_wg_sg[half_ + gl_SubgroupID] = sum;
    barrier();
    uint pre = 0u, tot = 0u;
    for (uint k = 0u; k < gl_NumSubgroups; ++k)
    {
        const uint t = s_wg_sg[half_ + k];
        pre += k < gl_SubgroupID ? t : 0u;
        tot += t;
    }
    total = tot;
    return pre + incl;
}

#endif
