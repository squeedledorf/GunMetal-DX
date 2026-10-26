// vkclean -- THE VISIBILITY QUEUE (#1039 M1, RenderVKVisQueue bit 1; LLCleanRendererVK::visQueue*,
// llcleanvisqueuevk.cpp): a consumer's PRODUCER stage writes compact visibility REQUESTS instead of walking
// them inline, ONE lean walk kernel (clean_deferred_light.comp VIS WALK: nothing but the walk is live)
// traces every request, and the consumer's RESOLVE stage reads the results. M1's consumers are the RT hit
// shading's SUN rays: the GI's HIT-MAT -> WALK -> HIT-LIGHT and the RT reflections' MAT -> WALK -> LIGHT
// (the split hit shading of #1036; the sun walk was the register peak of both lighting stages).
// (#1039 M2, RenderVKVisQueue bit 2; -DCLEAN_VIS_QUEUE_M2) the rest of the hit shading's walks leave the
// lighting kernels too: the RT reflections' hit OCCLUSION ray (VQ_K_OCC), the hit shadow-ray budget's K
// projector / point-light rays (VQ_K_LIGHT: the producer runs the local-light walk unshadowed -- its sum and
// the K picked shares --, the walk turns each share into the part its ray finds blocked, the resolve
// subtracts them) and, in a stage of its own (the RT split's LAYERS: one lane per entry, no request), the
// blend-layer gather with its per-layer sun rays. The lighting kernels then hold no ray query.
//
// ONE BUFFER, one REGION per consumer (0 the RT reflections, 1 the GI), found by device address: push
// rtr3.zw = the region's base (no descriptor). A region:
//   [0]      x the requests appended this frame, yzw the WALK's indirect dispatch (64 lanes per workgroup in
//            VQ_ROW-wide rows, 1) -- the host writes (0, 0, 0, 1) before the producer, the producer's appends
//            raise it (atomicMax: monotone in the end)
//   [1]      x the request capacity, y the ENTRY capacity (the consumer's list entries the queue serves:
//            entry e < y; the rest take the consumer's fused stage as a TAIL dispatch -- the queue never
//            drops a ray), z the result SLOTS per entry S (M1: 1), w the hit shadow-ray budget K the slots
//            were sized for (M2) (host)
//   [2]      RT region: xy the RT reflection records' device address, z their capacity (host)
//   [3]      the producer's counters: x entries that wanted a sun ray, y of them a constant (no ray: shadows
//            off / self-shadowed), z entries past the entry capacity (the tail's), w requests refused at
//            the request capacity (never, by construction: the host sizes it for the most an entry asks)
//   [4]      (M2) the producer's counters: x occlusion requests, y budgeted light requests, z entries whose
//            local lights the producer walked, w 0
//   [5..15]  reserved
//   [16 ..)  the REQUESTS, VQ_REQ_WORDS uvec4 each (48 B, exact: the walk kernel traces bit for bit what
//            the inline call would have)
//   [16 + 3 x request capacity ..)  the RESULTS: S uvec4 SLOTS per ENTRY (entry e's slot k at e x S + k --
//            a request's dest is that index), written by the producer (a constant / what a request's walk
//            needs) or the walk
// SLOTS (M1: the sun only, S = 1; M2: S = 2 + K, + 2 in the RT region):
//   VQ_SLOT_SUN        xyz the sun / moon visibility x tint (f32), w VQ_RES_*
//   VQ_SLOT_SUM        (M2) xyz the local lights' sum UNSHADOWED (RT_SHADE_BUDGET_OUT: every share in), w the
//                      budget's rays n (<= K): slots VQ_SLOT_K0 .. + n - 1 hold their blocked parts
//   VQ_SLOT_K0 + j     (M2) the producer writes pick j's shadowable share (xyz, f32), the walk replaces it
//                      with share x (1 - visibility): the part of it the ray finds blocked
//   vqSlotOcc (RT)     (M2) x the occlusion ray's visibility, y its blocker distance (rt_block_t), w VQ_RES_*
//   vqSlotLayers (RT)  (M2) the producer writes w = 1 where a sun ray narrowed the alpha-test cone (the
//                      blend layers' LOD, as in the fused LIGHT), the LAYERS stage replaces it with the
//                      layers' composite: xyz their light, w their transmittance T (f32 bits; T < 1: layers met)
// REQUEST (3 x uvec4):
//   q0  origin.xyz (f32, absolute region space as the TLAS), tmax
//   q1  direction.xyz (unit, f32), the alpha test's ray-cone width (rt_cone_width0)
//   q2  x dest (the result slot), y VQ_K_* kind | VQ_F_* flags | the light index << 16 (VQ_F_FIXTURE),
//       z the cone's anisotropy (rt_cone_aniso, f32 bits), w its spread (rt_cone_spread, f32 bits; 0 for a
//       sun ray)
// Requires GL_EXT_buffer_reference (+ uvec2), subgroup ballot, the draw push (g_draw_push).
#ifndef CLEAN_VIS_QUEUE_GLSL
#define CLEAN_VIS_QUEUE_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
layout(buffer_reference, std430, buffer_reference_align = 16) buffer VqRegion { uvec4 q[]; };

#define VQ_HDR_WORDS 16u
#define VQ_REQ_WORDS 3u
#define VQ_ROW       65535u   // (the WALK dispatch's row width: maxComputeWorkGroupCount's floor)
// request kinds (q2.y bits 0-3)
#define VQ_K_SUN     1u       // the single sun / moon ray with its tails (clean_vis_walk.glsl rtSunRayVis)
#define VQ_K_OCC     2u       // (M2) the RT reflection hit's occlusion ray: the mixed grey walk over every world
                              // blocker (traceShadowRay, RT_MASK_WORLD_BLOCK; rtrHitEnv)
#define VQ_K_LIGHT   3u       // (M2) a budgeted light's ray (rtrProjRay: the coloured walk over the opaque set)
// request flags (q2.y bits 8-15)
#define VQ_F_OPAQUE  256u     // rtSunRayVis opaque_only (the force-opaque walk)
#define VQ_F_SPLIT   512u     // the translucent split (RenderVKRTTranslucentSplit)
#define VQ_F_PARITY  1024u    // LL's blend-caster thresholds (RenderVKRTBlendShadowParity)
#define VQ_F_RGB     2048u    // coloured translucency (#212, RenderVKTranslucentShadows)
#define VQ_F_FOLIAGE 4096u    // inside a sun / moon trace: the foliage rules (#735; rtFoliageSet)
#define VQ_F_COARSE  8192u    // blend casters at their texture's mean (RenderVKProbeRTBlendCoarse)
#define VQ_F_FIXTURE 16384u   // (M2) the light's own fixture neither shadows nor tints (rt_fix_*; light q2.y >> 16)
// result flags (w)
#define VQ_RES_WALKED 1u      // the walk kernel traced it
#define VQ_RES_CONST  2u      // the producer's constant (no ray)
// result slots (see SLOTS)
#define VQ_SLOT_SUN  0u
#define VQ_SLOT_SUM  1u
#define VQ_SLOT_K0   2u
#define VQ_K_MAX     4u       // (= RT_SHADE_PROJ_MAXK)

VqRegion vqRegion() { return VqRegion(g_draw_push.rtr3.zw); }
uint vqReqCap(VqRegion r)   { return r.q[1].x; }
uint vqEntryCap(VqRegion r) { return r.q[1].y; }
uint vqSlots(VqRegion r)    { return max(r.q[1].z, 1u); }
uint vqBudgetK(VqRegion r)  { return min(r.q[1].w, VQ_K_MAX); }
uint vqSlotOcc(VqRegion r)    { return VQ_SLOT_K0 + vqBudgetK(r); }        // (the RT region's)
uint vqSlotLayers(VqRegion r) { return VQ_SLOT_K0 + vqBudgetK(r) + 1u; }   // (the RT region's)
// entry e's slot k (a request's dest)
uint vqDest(VqRegion r, uint e, uint k) { return e * vqSlots(r) + k; }
uint vqResWord(VqRegion r, uint dest) { return VQ_HDR_WORDS + VQ_REQ_WORDS * r.q[1].x + dest; }

// A constant result (no ray): vis = the visibility x tint the resolve reads.
void vqStoreConst(VqRegion r, uint dest, vec3 vis)
{
    r.q[vqResWord(r, dest)] = uvec4(floatBitsToUint(vis), VQ_RES_CONST);
}
// The producer's append: every active lane calls it once (want: this lane has a request). One atomic per
// subgroup (its lanes take consecutive slots: a warp's rays stay together -- spatial coherence for the
// walk), the electing lane raises the WALK's indirect dispatch to cover the new end. No cross-subgroup
// scan (no REQUIRE_FULL_SUBGROUPS). A slot past the request capacity (never: the host sizes it for the most
// requests an entry can make) gets a constant unshadowed result and is counted. cone / spread / aniso: the
// alpha test's ray cone for the walk (rt_cone_*).
void vqPush(VqRegion r, bool want, uint dest, vec3 o, float tmax, vec3 d, float cone, uint kindFlags, float aniso,
            float spread)
{
    const uvec4 b = subgroupBallot(want);
    const uint n = subgroupBallotBitCount(b);
    uint base = 0u;
    if (subgroupElect() && n != 0u)
    {
        base = atomicAdd(r.q[0].x, n);
        const uint groups = (min(base + n, vqReqCap(r)) + 63u) / 64u;
        atomicMax(r.q[0].y, min(groups, VQ_ROW));
        atomicMax(r.q[0].z, (groups + VQ_ROW - 1u) / VQ_ROW);
    }
    base = subgroupBroadcastFirst(base);   // (the electing lane's: before any lane leaves)
    if (!want) return;
    const uint slot = base + subgroupBallotExclusiveBitCount(b);
    if (slot >= vqReqCap(r))
    {
        atomicAdd(r.q[3].w, 1u);
        // (unshadowed: lit, nothing of a budgeted light's share blocked)
        vqStoreConst(r, dest, (kindFlags & 15u) == VQ_K_LIGHT ? vec3(0.0) : vec3(1.0));
        return;
    }
    const uint w = VQ_HDR_WORDS + slot * VQ_REQ_WORDS;
    r.q[w]      = uvec4(floatBitsToUint(o), floatBitsToUint(tmax));
    r.q[w + 1u] = uvec4(floatBitsToUint(d), floatBitsToUint(cone));
    r.q[w + 2u] = uvec4(dest, kindFlags, floatBitsToUint(aniso), floatBitsToUint(spread));
}
// The producer's counters ([3]: one atomic per subgroup and counter; every active lane calls it once).
void vqCount(VqRegion r, uint wanted, uint consts, uint past)
{
    const uint sw = subgroupAdd(wanted), sc = subgroupAdd(consts), sp = subgroupAdd(past);
    if (subgroupElect())
    {
        if (sw != 0u) atomicAdd(r.q[3].x, sw);
        if (sc != 0u) atomicAdd(r.q[3].y, sc);
        if (sp != 0u) atomicAdd(r.q[3].z, sp);
    }
}
// (M2) [4]'s counters, likewise.
void vqCount2(VqRegion r, uint occ, uint lights, uint walked)
{
    const uint so = subgroupAdd(occ), sl = subgroupAdd(lights), sh = subgroupAdd(walked);
    if (subgroupElect())
    {
        if (so != 0u) atomicAdd(r.q[4].x, so);
        if (sl != 0u) atomicAdd(r.q[4].y, sl);
        if (sh != 0u) atomicAdd(r.q[4].z, sh);
    }
}
// The resolve's read: result slot dest (xyz).
vec3 vqResult(VqRegion r, uint dest)
{
    return uintBitsToFloat(r.q[vqResWord(r, dest)].xyz);
}

#endif
