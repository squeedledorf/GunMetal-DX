// vkclean -- RAY-TRACED RECEIVER PROBES shared by the shadow consumers (#1048; #1039 design: RECV). Each
// is a short probe ray along the camera ray through a receiver rebuilt from depth, run once per receiver.
// Needs clean_frame_info.glsl (ssr.cluster_rt) and clean_rt_common.glsl (the RT set) before inclusion.
// Users: the RT shadow mask, RTAO, pass 15 (the generic core and the merged sun trace).
#ifndef CLEAN_RT_RECEIVER_GLSL
#define CLEAN_RT_RECEIVER_GLSL

// #747 interim CLUSTER LOD RECEIVER LIFT: an avatar the cluster path draws can still cast from its LEGACY
// rigged geometry (llcleanclusterrtvk.h RIGGED: RenderVKClusterRTRigged 0, its cluster RT data not
// ready), while the pre-pass depth the receiver is rebuilt from is its CLUSTER surface. The two differ
// by at most the cut's error, eps = tol_px x max(distance, znear) / px_scale (the selection's own
// projection bound: llcleanclustervk.h shadowLiftBound): where the cluster surface lies inside the
// legacy hull, every ray leaving it hit that hull -- hard-edged self-shadow patches along the crossing
// curves, moving with the pose (every AO ray at once). A receiver within eps of such a legacy surface (a
// short camera-ray probe over the rigged classes, +-2 eps around P, any face flagged
// RT_GEOM_W_CLUSTER_RIG) gets its ray origins lifted by eps along the light-facing normal: out of its own
// legacy hull, so only occluders nearer than the LOD's own error go unresolved. Returns the lift (m),
// 0 = none (LOD off, nothing switched, no flagged surface near: the previous behaviour exactly).
// (Before #1048 three copies: the mask's, RTAO's and pass 15's; only the last guarded the probe ray.)
float rtClusterRigLift(vec3 P, vec3 cam)
{
    if (ssr.cluster_rt.z < 0.5 || !(ssr.cluster_rt.x > 0.0)) return 0.0;
    // GATE (#748 perf pass): TLAS pass B counted no switched face traced on its legacy geometry
    // this frame (word 2 of the RT skinned arena: bit 31 valid | the count, LLCleanClusterRTVK::
    // recordLiftFlag; 0 = unknown -- no rigged cluster RT / no group map: the probe runs). Every
    // switched avatar then casts from its cluster cut and there is no legacy hull to leave.
    if (uint(rt_cl_rig.length()) > 2u && rt_cl_rig[2] == 0x80000000u) return 0.0;
    vec3 V = P - cam;
    const float camd = length(V);
    if (camd < 1e-4) return 0.0;
    V /= camd;
    const float eps = ssr.cluster_rt.x * max(camd, ssr.cluster_rt.y);
    const float w = 2.0 * eps + 1e-3;
    vec3 qo = P - V * w;
    float qt0 = 0.0, qt1 = 2.0 * w;
    if (!rtRaySane(qo, qt0, V, qt1)) return 0.0;   // (Xid 109: an invalid probe ray)
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTLAS, gl_RayFlagsNoOpaqueEXT, RT_MASK_RIGGED | RT_MASK_SELF, qo, qt0, V, qt1);
    uint n_cand = 0u;
    bool hit = false;
    while (rayQueryProceedEXT(rq))
    {
        if (rayQueryGetIntersectionTypeEXT(rq, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        if (++n_cand > RT_CANDIDATE_BUDGET) { rayQueryTerminateEXT(rq); break; }
        const uint id = rtHitInstance(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),
                                      uint(rayQueryGetIntersectionGeometryIndexEXT(rq, false)));
        if (rtIsClusterRig(id)) { hit = true; rayQueryTerminateEXT(rq); break; }
    }
    return hit ? eps : 0.0;
}

#endif
