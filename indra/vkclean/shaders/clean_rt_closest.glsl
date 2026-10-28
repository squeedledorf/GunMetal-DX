// vkclean -- THE CLOSEST-HIT WALK (#1048): the nearest surface along a camera-like ray with the RASTER's
// rules -- alpha-mask holes honoured (THE alpha-test rule, clean_alpha_rule.glsl, through
// rtCandidateAlphaCol at the caller's ray cone), single-sided back faces skipped (a candidate one in the
// shader, a committed opaque one by re-tracing past it: up to 3 traversals). One body for the RT
// reflection / GI / pass 14 walks (clean_deferred_light.comp rtrClosest) and the probe RT capture
// (clean_probe_rt.comp probeClosest), which had copied it. Included where those live, after
// clean_rt_common.glsl; the includer's extras come in through hooks defined BEFORE the include
// (each defaults to nothing):
//   RTC_BEGIN()                            a walk starts (per-walk state)
//   RTC_TRAVERSE()                         a traversal starts (cost counters)
//   RTC_OVER_BUDGET()                      a traversal hit the candidate budget
//   RTC_CANDS(n)                           the candidates a traversal handled
//   RTC_KEEP(id, fl, a, cutoff, blend, keep)  adjust / count a candidate's verdict (keep: inout bool)
//   RTC_BACKFACE(h, d, rflags, stop)       a committed single-sided back face: stop = true returns h as
//                                          the hit (a back-face STOP); may change rflags for the re-trace
//   RTC_RETRACE(k)                         a back-face re-trace after traversal k
// (clean_rt_common.glsl traceClosestHit, the debug views' walk, keeps its own rules: no re-trace,
// optional NoOpaque facing.)
#ifndef CLEAN_RT_CLOSEST_GLSL
#define CLEAN_RT_CLOSEST_GLSL

#ifndef RTC_BEGIN
#define RTC_BEGIN()
#endif
#ifndef RTC_TRAVERSE
#define RTC_TRAVERSE()
#endif
#ifndef RTC_OVER_BUDGET
#define RTC_OVER_BUDGET()
#endif
#ifndef RTC_CANDS
#define RTC_CANDS(n)
#endif
#ifndef RTC_KEEP
#define RTC_KEEP(id, fl, a, cutoff, blend, keep)
#endif
#ifndef RTC_BACKFACE
#define RTC_BACKFACE(h, d, rflags, stop)
#endif
#ifndef RTC_RETRACE
#define RTC_RETRACE(k)
#endif

// An avatar's face: rigged (bit 31 of its geometry row), worn (512) or the agent's own (8192).
bool rtIsAvatar(uint id)
{
    if (!rtIdIn(id)) return false;   // (#874 guard)
    const uint f = rt_instances[id].flags;
    return (rt_geom[id].z & 0x80000000u) != 0u || (f & (512u | 8192u)) != 0u;
}

// The walk's per-call rules (data, uniform per call site):
//   mask      TLAS classes
//   rflags    extra ray flags (the reflection rays' hardware back-face culling)
//   blendMin  an ALPHA-BLEND candidate is a hit at alpha >= blendMin (blendGt: > blendMin -- "anything
//             shows", ALPHA_VISIBLE_EPS); alpha-mask candidates take their material cutoff
//   noAvatars avatar faces (rtIsAvatar) are skipped as candidates (a capture without its Dynamic flag)
struct RtClosestRules
{
    uint  mask;
    uint  rflags;
    float blendMin;
    bool  blendGt;
    bool  noAvatars;
};
RtHit rtClosestWalk(vec3 o, vec3 d, float tmin, float tmax, RtClosestRules r)
{
    RtHit h;
    h.hit = false;
    RTC_BEGIN();
    uint rflags = r.rflags;
    for (int k = 0; k < 3; ++k)
    {
        // (Xid 109) a back face committed within max(dist x 1e-5, 1e-4) of tmax steps the re-trace's
        // tmin PAST tmax -- an invalid ray query (tmin <= tmax is required; undefined otherwise, a
        // non-terminating traversal on NVIDIA). Nothing is left on the interval: a miss. And a ray
        // the caller built from non-finite inputs (rtRaySane): a miss.
        if (!(tmin < tmax)) return h;
        RT_BC_SITE(RT_BC_SITE_CLOSEST | (uint(k) << 8) | (r.mask & 0xFFu));   // (#1128 hang beacon variants only)
        if (!rtRaySane(o, tmin, d, tmax)) return h;
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, uTLAS, rflags, r.mask & 0xFFu, o, tmin, d, tmax);
        RTC_TRAVERSE();
        uint n_cand = 0u;
        while (rayQueryProceedEXT(rq))
        {
            if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
            if (++n_cand > RT_CANDIDATE_BUDGET)
            {   // over budget (G-7): the nearest hit committed so far stands
                RTC_OVER_BUDGET();
                rayQueryTerminateEXT(rq);
                break;
            }
            const uint id   = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                            uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
            const uint prim = RT_HIT_PRIM(rq, false, id);
            const vec2 bc   = rayQueryGetIntersectionBarycentricsEXT(rq, false);
            const float t   = rayQueryGetIntersectionTEXT(rq, false);
            if (r.noAvatars && rtIsAvatar(id)) continue;
            const uint fl = rtIdIn(id) ? rt_instances[id].flags : 256u;   // (#874 guard: alpha test -> opaque)
            if ((fl & (256u | 2048u)) == 0u && !rtCandidateFrontQ(id, prim, d, rq)) continue;
            float cutoff; bool blend; vec3 col;
            const float a = rtCandidateAlphaCol(id, prim, bc, t, d, rq, cutoff, blend, col);
            bool keep = blend ? (r.blendGt ? a > r.blendMin : a >= r.blendMin) : a >= cutoff;
            RTC_KEEP(id, fl, a, cutoff, blend, keep);
            if (keep) rayQueryConfirmIntersectionEXT(rq);
        }
        RTC_CANDS(n_cand);
        if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT)
            return h;   // miss
        h.hit = true;
        h.dist = rayQueryGetIntersectionTEXT(rq, true);
        h.instance = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)),
                                   uint(rayQueryGetIntersectionGeometryIndexEXT(rq, true)));
        if (!rtIdIn(h.instance)) { h.hit = false; return h; }   // (#874 guard: a bad id is a miss)
        h.prim = RT_HIT_PRIM(rq, true, h.instance);
        h.bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
        h.front = rayQueryGetIntersectionFrontFaceEXT(rq, true);
        h.o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
        // Opaque instances never ran the candidate test: the raster back-face cull for single-sided
        // ones -> continue past this triangle (or stop at it: the includer's back-face STOP).
        const uint fl = rt_instances[h.instance].flags;
        if ((fl & (256u | 2048u)) != 0u || rtCandidateFront(h.instance, h.prim, d, h.o2w)) return h;
        bool stop = false;
        RTC_BACKFACE(h, d, rflags, stop);
        if (stop) return h;
        tmin = h.dist + max(h.dist * 1e-5, 1e-4);
        h.hit = false;
        RTC_RETRACE(k);
    }
    return h;
}

#endif
