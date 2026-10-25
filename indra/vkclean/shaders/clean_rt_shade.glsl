// vkclean -- RAY-TRACED HIT SHADING: lights an RtSurface (clean_rt_surface.glsl) the way
// clean_draw.frag / clean_water.frag shade a REFLECTION-PROBE CAPTURE fragment
// (probeCapture(): sun_color.w = 1). That specialization of the lit FS:
//   * no probe sampling (sampleProbes returns false in captures): irradiance = the sky
//     ambient (amblit), radiance = skyColor(R);
//   * no SSR, planar mirror, AO, debug views, lit-debug switches or far-clip fade;
//   * local lights = the capture's own culled list (cluster CLUSTER_COUNT + list, written
//     by clean_probe_gpu.comp PH_LIGHTS, #478), projector shadows: shadow-map records take the
//     atlas (filter quality 0 = one compare tap, as captures do), ray-traced records are
//     unshadowed (captures never trace projector rays);
//   * haze / water fog from the capture eye (the probe origin).
// The one deliberate difference: the SUN / MOON visibility is ONE hard shadow ray against
// the TLAS (alpha-tested casters, translucent tint per RenderVKTranslucentShadows) at every
// hit facing the light. The raster face takes the cascades (only where they cover the
// MAIN camera's range) or, with the ray-traced sun, the soft multi-ray trace.
//
// Every BRDF term is the lit FS's own (clean_brdf.glsl); keep the branch structure in
// step with clean_draw.frag::shade when that changes.
//
// RT_SHADE_NO_HAZE (#87, the GI trace): no haze / water fog between the receiver and the hit -- the
// haze's in-scatter is sky light, which a GI ray inside a sealed room must not bring in (its
// bounces fed it back through the radiance cache: a lit-looking sealed room).
// MAIN-VIEW HOOKS (#859, the RT reflection pass in clean_deferred_light.comp; undefined in the
// probe capture, which keeps the capture rules above):
//   RT_SHADE_HAZE_K            -> a factor on the haze IN-SCATTER (sky light) between the receiver and
//                                 the hit, read after RT_SHADE_ENV (the reflections under GI: the hit's
//                                 occlusion ray -- a sealed room's air is not sky-lit); default 1
//   RT_SHADE_LIGHT_CLUSTER(P)  -> the light cluster whose list rtHitLocalLights walks (instead
//                                 of the capture's list CLUSTER_COUNT + list); RT_SHADE_ALL_LIGHTS
//                                 = walk every light of the frame (a hit outside the main view's
//                                 cluster grid: each light's own range test decides);
//   RT_SHADE_PROJ_RAY(L, P, N) -> the visibility of a RAY-TRACED projector record at the hit (one
//                                 hard shadow ray; the capture leaves those unshadowed);
//   RT_SHADE_ENV(P, N, R, rough, legacy, amblit, irr, rad, rad2) -> the environment at the hit:
//                                 irradiance, radiance along R at the roughness's lod and at
//                                 lod 0 (instead of amblit / skyColor(R)).
//   RT_SHADE_PROJ_K            -> (with RT_SHADE_PROJ_RAY) the HIT SHADOW-RAY BUDGET: at most this
//                                 many ray-traced projector records per hit get RT_SHADE_PROJ_RAY
//                                 (0 .. RT_SHADE_PROJ_MAXK, run-time uniform). Every contributing
//                                 record is shaded unshadowed in the walk; the K whose shadowable
//                                 share is the brightest are kept (insertion) and, after the walk,
//                                 traced once each and their share scaled by the visibility. The
//                                 rest stay unshadowed (the capture rule). Undefined: every RT
//                                 projector record takes its own ray inside the walk (the old
//                                 behaviour -- dozens of shadow rays per hit under a stage rig).
//   RT_SHADE_STAT(slot, n)     -> stats hook (the RT trace's counters): slot 0 lights in range at
//                                 a hit, 1 RT projector records wanting a shadow ray, 2 rays cast.
//   RT_SHADE_REGIR(P, N, pl, pw) -> (#869) the WORLD-SPACE LIGHT GRID (clean_regir.glsl): the lights
//                                 to evaluate at the hit instead of the cluster walk -- returns how
//                                 many (light index, weight) pairs it wrote into pl / pw (<=
//                                 REGIR_PICKS_MAX; the cell's exact list with weight 1, or the RIS
//                                 picks with their unbiased weights), or REGIR_NO_CELL = walk the
//                                 cluster as without the hook. Each picked light is evaluated in
//                                 full (its current record) and its share scaled by the weight;
//                                 the shadow-ray budget ranks the weighted shares.
//   RT_SHADE_LIGHT_NOTE(li)    -> stats hook: light li is evaluated at this hit (in range).
//
// Requires before inclusion: clean_lights_common.glsl + the light buffers (set 0
// bindings 3-6: lights, cluster_count, cluster_index, linfo), clean_frame_info.glsl,
// clean_atmos_common.glsl, uTextures, clean_rt_common.glsl, clean_brdf.glsl,
// clean_light_eval.glsl, clean_projshadow.glsl (+ uProjShadowAtlas / uProjTransDepth /
// uProjTransColor at set 0 bindings 18 / 29 / 30), clean_shadow_rt.glsl,
// clean_rt_surface.glsl.
#ifndef CLEAN_RT_SHADE_GLSL
#define CLEAN_RT_SHADE_GLSL
#ifndef RT_SHADE_STAT
#define RT_SHADE_STAT(slot, n)
#endif
#ifndef RT_SHADE_LIGHT_NOTE
#define RT_SHADE_LIGHT_NOTE(li)
#endif
#if defined(RT_SHADE_PROJ_K) && defined(RT_SHADE_PROJ_RAY)
#define RT_SHADE_PROJ_BUDGETED 1
#ifndef RT_SHADE_PROJ_MAXK
#define RT_SHADE_PROJ_MAXK 4
#endif
#else
#define RT_SHADE_PROJ_BUDGETED 0
#endif

// Projector shadow-map visibility at filter quality 0 (clean_draw_common.glsl
// projShadowMap, the capture path) + the atlas translucency layer.
vec3 rtProjShadowMapQ0(Light L, uint idx, vec3 P, vec3 N)
{
    ProjShadow s = pshadow[idx];
    if (s.params.w < 0.5) return vec3(1.0);
    vec3  tl    = L.proj_origin.xyz - P;
    float d     = max(dot(-tl, L.proj_axis.xyz), 0.0);
    vec3  l     = normalize(tl);
    float texel = s.params.x * d;
    float k     = 1.0 - clamp(dot(N, l), 0.0, 1.0);
    vec3  Po    = P + N * (texel * s.texel.z * (0.5 + k)) + l * (texel * s.texel.w);
    vec4  c     = s.vp * vec4(Po - ssr.cam_origin.xyz, 1.0);   // camera-relative (#555)
    if (c.w <= 0.0) return vec3(1.0);
    vec3  ndc   = c.xyz / c.w;
    if (ndc.z >= 1.0 || ndc.z <= 0.0) return vec3(1.0);
    vec2  uv    = ndc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec3(1.0);
    vec2  t     = s.texel.xy;
    vec2  lo    = s.rect.xy + t * 0.5, hi = s.rect.xy + s.rect.zw - t * 0.5;
    vec2  a     = s.rect.xy + uv * s.rect.zw;
    const float vis = textureLod(uProjShadowAtlas, vec3(clamp(a, lo, hi), ndc.z), 0.0);
    if (vis > 0.001 && ssr.render_opts.w > 0.5)
    {
        const vec2 t2 = t * 2.0;
        const vec2 tc = clamp(a, s.rect.xy + t2, s.rect.xy + s.rect.zw - t2);
        const float front = textureLod(uProjTransDepth, vec3(tc, ndc.z), 0.0);
        if (front < 0.999) return vis * mix(textureLod(uProjTransColor, tc, 0.0).rgb, vec3(1.0), front);
    }
    return vec3(vis);
}

// clean_draw_common.glsl localLights, probe-capture specialization (list = the capture's
// light list: 0 scheduled job, 1 realtime probe).
vec3 rtHitLocalLights(vec3 P, vec3 N, vec3 V, bool pbr, vec3 base, float metal, float rough, LegacySpec ls, uint list)
{
    const uint count = uint(linfo.cam_pos.w);
    if (count == 0u) return vec3(0.0);
#ifdef RT_SHADE_REGIR
    // (#869) the light grid's picks (weighted) instead of the walk, where the hook takes the hit
    uint  rgL[REGIR_PICKS_MAX];
    float rgW[REGIR_PICKS_MAX];
    const uint rgN = RT_SHADE_REGIR(P, N, rgL, rgW);
    const bool useGrid = rgN != REGIR_NO_CELL;
#endif
#ifdef RT_SHADE_LIGHT_CLUSTER
    uint c = 0u;
    bool walkAll = false;
    uint n = 0u;
#ifdef RT_SHADE_REGIR
    if (useGrid) n = rgN;
    else
#endif
    {
        c = RT_SHADE_LIGHT_CLUSTER(P);
        walkAll = c == RT_SHADE_ALL_LIGHTS;
        n = walkAll ? min(count, 256u) : clusterLen(cluster_count[c]);
    }
#else
    const uint c = CLUSTER_COUNT + list;
    uint n = clusterLen(cluster_count[c]);
#ifdef RT_SHADE_REGIR
    if (useGrid) n = rgN;
#endif
#endif
    const vec3 diffuseColor  = base * (1.0 - 0.04) * (1.0 - metal);
    const vec3 specularColor = mix(vec3(0.04), base, metal);
    vec3 sum = vec3(0.0);
#if RT_SHADE_PROJ_BUDGETED
    // HIT SHADOW-RAY BUDGET (see RT_SHADE_PROJ_K): the K brightest shadowable projector shares,
    // descending (score 0 = empty slot).
    const uint projK = min(uint(RT_SHADE_PROJ_K), uint(RT_SHADE_PROJ_MAXK));
    float kScore[RT_SHADE_PROJ_MAXK];
    uint  kLight[RT_SHADE_PROJ_MAXK];
    vec3  kShare[RT_SHADE_PROJ_MAXK];
    [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j) { kScore[j] = 0.0; kLight[j] = 0u; kShare[j] = vec3(0.0); }
#endif
    for (uint i = 0u; i < n; ++i)
    {
        uint li;
        float lw = 1.0;   // the light's estimator weight (1: exact; the grid's RIS weight)
#ifdef RT_SHADE_REGIR
        if (useGrid) { li = rgL[i]; lw = rgW[i]; }
        else
#endif
        {
#ifdef RT_SHADE_LIGHT_CLUSTER
            li = walkAll ? i : cluster_index[c * CLUSTER_CAP + i];
#else
            li = cluster_index[c * CLUSTER_CAP + i];
#endif
        }
        if (li >= count) continue;   // (never: a list entry past this frame's lights)
        const vec4 pr = lights[li].pos_radius;
        vec3 lv = pr.xyz - P;
        const float d2 = dot(lv, lv);
        if (d2 >= pr.w * pr.w) continue;
        Light L = lights[li];
        const float dist = sqrt(d2);
        if (dist >= L.pos_radius.w) continue;
        const float atten = legacyAtten(dist / L.pos_radius.w, L.color_falloff.w);
        if (atten <= 0.0) continue;
        RT_SHADE_STAT(0, 1u);
        RT_SHADE_LIGHT_NOTE(li);
        if (L.proj_origin.w < 0.5)
        {   // POINT (multiPointLightF)
            const vec3 l = lv / max(dist, 1e-4);
            vec3 pt = vec3(0.0);
            if (pbr)
            {
                float nl; vec3 df, sp;
                llPunctual(diffuseColor, specularColor, rough, N, V, l, nl, df, sp);
                pt = atten * L.color_falloff.rgb * 3.25 * clamp(nl * (df + sp), vec3(0.0), vec3(10.0));
            }
            else
            {
                const float nl = dot(N, l);
                if (nl > 0.0)
                {
                    pt = L.color_falloff.rgb * (nl * atten) * base;
                    if (ls.gloss > 0.0)   // legacy Blinn-Phong (#470, multiPointLightF)
                    {
                        float slit;
                        const float scol = legacySpecular(N, V, l, ls.gloss, slit);
                        pt += (slit * atten * scol) * L.color_falloff.rgb * ls.spec;
                    }
                }
            }
            sum += lw * pt;
#if RT_SHADE_PROJ_BUDGETED && defined(RT_SHADE_POINT_BUDGET)
            // (RT_SHADE_POINT_BUDGET, the GI trace) the point light's share competes for the hit
            // shadow-ray budget like a ray-traced projector record (RT_SHADE_PROJ_RAY traces toward
            // its centre, stopping at its fixture sphere); without the hook point lights stay
            // unshadowed (the capture rule)
            if (RT_SHADE_POINT_BUDGET && projK > 0u)
            {
                vec3  psh = lw * pt;
                float sc = dot(psh, vec3(0.2126, 0.7152, 0.0722));
                uint  sl = li;
                [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j)
                {
                    if (uint(j) < projK && sc > kScore[j])
                    {
                        const float ts = kScore[j]; const uint tl = kLight[j]; const vec3 tv = kShare[j];
                        kScore[j] = sc; kLight[j] = sl; kShare[j] = psh;
                        sc = ts; sl = tl; psh = tv;
                    }
                }
            }
#endif
        }
        else
        {   // PROJECTOR (spotLightF)
            vec4 ptc = L.proj_mat * vec4(P, 1.0);
            if (ptc.z < 0.0) continue;
            ptc.xyz /= ptc.w;
            if (ptc.z <= 0.0 || ptc.x <= 0.0 || ptc.x >= 1.0 || ptc.y <= 0.0 || ptc.y >= 1.0) continue;
            const float l_dist = -dot(lv, L.proj_axis.xyz);
            const vec3  l  = normalize(L.proj_origin.xyz - P);
            const float nl_raw = dot(N, l);
            const float nl = clamp(nl_raw, 0.000001, 1.0);
            vec3 dlit = projDiffuse(L, l_dist, ptc.xy);
            // Capture rule (projectorVisibility): shadow-map records sample the atlas,
            // ray-traced records stay unshadowed.
            const bool legacySpec = !pbr && ls.gloss > 0.0;
            const bool legacyEnv  = !pbr && ls.env > 0.0;
            vec3 vis = vec3(1.0);
            bool rayRec = false;   // (budgeted: a ray-traced record whose shadow is decided after the walk)
            // SHADOWED AMBIANCE (RenderVKProjAmbianceShadowedMode; localLight's rule): a shadowed
            // record's ambiance takes its visibility -- the soft max-LOD texel widens the test, and
            // a hit turned away from the apex (the legacy ambiance's only receivers) is shadowed
            // by its own surface under the back-face self-shadow rule (0, no ray).
            const bool ambSh = L.proj_params.w > 0.0 && projAmbShadowed() && L.tex.z != 0u
                            && pshadow[L.tex.z - 1u].params.w > 0.5;
            const bool ambBack = ambSh && !(nl_raw > 0.0);
            const bool ambSelf = ambBack && rtBackfaceSelfShadow();
            if (!ambSelf && (ambBack || (nl_raw > 0.0 && (ambSh || legacyEnv || max(max(dlit.r, dlit.g), dlit.b) * atten * (legacySpec ? 16.0 : 1.0) > 0.002)))
                && L.tex.z != 0u)
            {
                const uint idx = L.tex.z - 1u;
                if (pshadow[idx].params.w > 0.5 && pshadow[idx].params.z <= 1.5)
                    vis = rtProjShadowMapQ0(L, idx, P, N);
#ifdef RT_SHADE_PROJ_RAY
                else if (pshadow[idx].params.w > 0.5)
                {
                    RT_SHADE_STAT(1, 1u);
#if RT_SHADE_PROJ_BUDGETED
                    rayRec = true;
#else
                    RT_SHADE_STAT(2, 1u);
                    vis = RT_SHADE_PROJ_RAY(L, P, N);
#endif
                }
#endif
            }
            if (ambSelf) vis = vec3(0.0);   // (its own surface: no ray)
            dlit *= vis;
            // sh = the record's SHADOWABLE share (through dlit / vis); the ambiance takes the
            // shadow only when ambSh (a budgeted ray record: into sh unshadowed, the budget's ray
            // removes its blocked part; else x vis), otherwise straight into sum.
            const vec3 ambV = ambSh ? (rayRec ? vec3(1.0) : vis) : vec3(1.0);
            vec3 sh = vec3(0.0);
            const float amb_da = (nl * 0.5 + 0.5) * L.proj_params.w;
            if (pbr)
            {
                float nlp; vec3 df, sp;
                llPunctual(diffuseColor, specularColor, rough, N, V, l, nlp, df, sp);
                const vec3 brdf = clamp(nlp * (df + sp), vec3(0.0), vec3(10.0));
                sh += atten * dlit * 3.25 * brdf;
                const vec3 at = projAmbiance(L, amb_da, atten, 0.0, nl, ptc.xy) * 3.25 * brdf * ambV;
                if (ambSh) sh += at; else sum += lw * at;
            }
            else
            {
                const float lit = nl * atten;
                sh += dlit * lit * base;
                const vec3 at = base * projAmbiance(L, amb_da, atten, lit, nl, ptc.xy) * max(dot(-l, N), 0.0) * ambV;
                if (ambSh) sh += at; else sum += lw * at;
                if (legacySpec)   // legacy Blinn-Phong (#470, spotLightF)
                {
                    float slit;
                    const float scol = legacySpecular(N, V, l, ls.gloss, slit);
                    sh += clamp(dlit * (slit * atten * scol) * ls.spec, vec3(0.0), vec3(1.0));
                }
                if (legacyEnv && nl_raw > 0.0)   // projector image reflection (spotLightF)
                    sh += projEnvReflect(L, P, N, V, ls.gloss) * vis * ls.env;
            }
            sh *= lw;   // (the weighted share: what the sum holds and the budget ranks / removes)
            sum += sh;
#if RT_SHADE_PROJ_BUDGETED
            if (rayRec && projK > 0u)
            {   // keep the K brightest shares (descending insertion; the dimmest drops off the end)
                float sc = dot(sh, vec3(0.2126, 0.7152, 0.0722));
                uint  sl = li;
                [[unroll]] for (int j = 0; j < RT_SHADE_PROJ_MAXK; ++j)
                {
                    if (uint(j) < projK && sc > kScore[j])
                    {
                        const float ts = kScore[j]; const uint tl = kLight[j]; const vec3 tv = kShare[j];
                        kScore[j] = sc; kLight[j] = sl; kShare[j] = sh;
                        sc = ts; sl = tl; sh = tv;
                    }
                }
            }
#endif
        }
    }
#if RT_SHADE_PROJ_BUDGETED
    // The budget's rays, after the walk (one call site: one inlined shadow walk).
    for (uint j = 0u; j < projK; ++j)
    {
        if (kScore[j] <= 0.0) break;
        RT_SHADE_STAT(2, 1u);
        const vec3 vis = RT_SHADE_PROJ_RAY(lights[kLight[j]], P, N);
        sum -= kShare[j] * (vec3(1.0) - vis);
    }
    sum = max(sum, vec3(0.0));
#endif
    return sum;
}

// Sun / moon visibility at a hit: one hard ray toward the light (the RT sun's caster
// rules: every class but water, alpha test, translucent casters dim / tint). The origin
// leaves along the light-facing GEOMETRIC normal of the hit triangle by the float error
// of the hit point (clean_rt_common.glsl offsetRayOrigin -- the textbook Wachter &
// Binder case: P is the barycentric interpolation of the transformed triangle, no
// depth reconstruction) plus the user's extra offset RenderVKRTShadowBias (sun_rt.z,
// default 0). foot = the hit's footprint (m): the casters' alpha-test texture LOD.
// SHADOW TERMINATOR (#664): lift = the hit triangle's Hanika lift (RtSurface.tlift, on
// Ng's side -- the side the ray arrived from; 0 = none). Escape-checked here (one short
// force-opaque ray, only for hits in the sun's band that got a lift), then the origin
// stays on that side, above the facet, as the main view's sunShadowRayCoreImpl.
float rtHitSunShadowCore(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint);
// #719: the volumetric cloud layer's sun transmittance on top (clean_cloud_shadow_map.glsl;
// 1 while the layer is off), as the main view's sunShadowT -- captured probes see cloud shadows.
float rtHitSunShadow(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint)
{
    return rtHitSunShadowCore(P, Ng, L, dist, foot, lift, tint) * cloudShadowMap(P);
}
float rtHitSunShadowCore(vec3 P, vec3 Ng, vec3 L, float dist, float foot, vec3 lift, out vec3 tint)
{
    tint = vec3(1.0);
    if (ssr.shadow_params.w < 0.5) return 1.0;   // sun / moon shadows off
    vec3 n = Ng;
    if (dot(lift, lift) > 0.0)
        lift = rtTerminatorEscape(P, offsetRayOrigin(P, Ng), Ng, lift);
    const bool lifted = dot(lift, lift) > 0.0;
    // BACK-FACE SELF-SHADOW (clean_shadow_rt.glsl rtBackfaceSelfShadow; Ng is the hit's side, the
    // one the arriving ray sees): turned away from the sun, its own surface shadows it -- no ray
    // whose origin would leave through a single-plane wall
    if (!lifted && rtBackfaceSelfShadow()) { if (!(dot(n, L) > 0.0)) return 0.0; }
    else if (!lifted && dot(n, L) < 0.0) n = -n;
    const vec3 Po = offsetRayOrigin((lifted ? P + lift : P) + n * ssr.sun_rt.z, n);
    rt_cone_width0 = foot;
    rt_cone_spread = 0.0;
    const float tmax = ssr.sun_rt.w > 1.0 ? ssr.sun_rt.w : 512.0;
    // Translucent casters at their texture's mean (RenderVKProbeRTBlendCoarse): no UV /
    // LOD work per candidate; the capture's GGX filter erases texel detail anyway.
    rt_blend_coarse = (rtBlendOpts() & 4u) != 0u;
    // Translucent split (RenderVKRTTranslucentSplit, clean_shadow_rt.glsl): the opaque
    // set first (binary, cut-outs alpha-tested); the translucent transmittance ray only
    // where it is lit -- shadowed hits never walk the blend casters at all.
    // Foliage transmission (#735): the main view's rules, so captured canopies match.
    // (shadowDiscRay: the split's opaque walk or the mixed one -- one inlined walk.)
    rtFolBegin();
    const bool split = rtSplitOn();
    vec3 vis = shadowDiscRay(Po, L, 0.0, tmax, split);
    if (split && vis.x > 0.0) vis *= shadowRayTrans(Po, L, 0.0, tmax);
    vis = rtFolTransmit(vis, Po, L, tmax);
    rtFolEnd();
    rt_blend_coarse = false;
    const float sv = max(max(vis.r, vis.g), vis.b);
    if (sv > 1e-4) tint = vis / sv;
    return sv;
}

// Water normal-map LOD for a world-space UV scale k (uv per metre) at the hit footprint.
float rtWaterLod(int idx, float k)
{
    if (idx < 0 || rt_surf_foot <= 0.0) return 0.0;
    const vec2 tsz = vec2(textureSize(uTextures[nonuniformEXT(uint(idx))], 0));
    const float levels = float(textureQueryLevels(uTextures[nonuniformEXT(uint(idx))]));
    return clamp(log2(max(rt_surf_foot * k * max(tsz.x, tsz.y), 1e-8)), 0.0, max(levels - 1.0, 0.0));
}
vec3 rtWaterNormalTex(int idx, vec2 uv, float lod)
{
    if (idx < 0) return vec3(0.0, 0.0, 1.0);
    return textureLod(uTextures[nonuniformEXT(uint(idx))], uv, lod).xyz * 2.0 - 1.0;
}

// The reflection direction of the water surface at P seen from eye (clean_water.frag's wave_ibl
// and R: the three wave layers, the frame's wave scale; #859 RT forward reflections). The
// footprint LOD is rt_surf_foot's (the caller's last rtSurfaceEval).
vec3 rtWaterReflectDir(vec3 P, vec3 eye, float time_s)
{
    const float time = time_s * 0.5;
    const vec3 viewVec = normalize(P - eye);
    vec2 wave1d = ssr.water[0].xy, wave2d = ssr.water[0].zw;
    vec3 v = P;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 bigWave = v.xy * vec2(0.04, 0.04) + wave1d * time * 0.055;
    vec4 littleWave;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + wave2d * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2)  + wave1d * time * 0.1;
    const int ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    const float blend = ssr.water[1].w;
    const vec3 wave1 = mix(rtWaterNormalTex(ia, bigWave, rtWaterLod(ia, 0.04)),       rtWaterNormalTex(ib, bigWave, rtWaterLod(ib, 0.04)),       blend);
    const vec3 wave2 = mix(rtWaterNormalTex(ia, littleWave.xy, rtWaterLod(ia, 0.9)), rtWaterNormalTex(ib, littleWave.xy, rtWaterLod(ib, 0.9)), blend);
    const vec3 wave3 = mix(rtWaterNormalTex(ia, littleWave.zw, rtWaterLod(ia, 0.2)), rtWaterNormalTex(ib, littleWave.zw, rtWaterLod(ib, 0.2)), blend);
    const vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;
    vec3 wave_ibl = wavef * ssr.water[1].xyz;
    wave_ibl.z *= 2.0;
    wave_ibl = normalize(wave_ibl);
    vec3 R = reflect(viewVec, wave_ibl);
    R.z = abs(R.z);
    return normalize(R);
}

// clean_water.frag, WATER_OPAQUE build (the capture water): waves, Fresnel, sky
// reflection, sun glint, fully fogged body; haze from the eye. time = draw time (s).
// In two halves around the glint's sun ray: rtShadeHit traces the sun ONCE for every surface model
// (one inlined sun walk -- the water's own call site was a second copy of the opaque / translucent /
// foliage walks in every kernel that shades hits).
struct RtWaterShade
{
    vec3 color;      // under water: the final colour (done)
    vec3 punctual;   // the sun glint before its visibility
    vec3 radiance;   // the reflection x df2.y
    vec3 fb;
    float mixK;      // min(1, df2.x)
    vec3 additive, atten;
    bool done;       // no glint ray, color is final
};
RtWaterShade rtShadeWaterBegin(vec3 P, vec3 eye, float time_s)
{
    RtWaterShade w;
    w.color = vec3(0.0); w.punctual = vec3(0.0); w.radiance = vec3(0.0); w.fb = vec3(0.0); w.mixK = 0.0;
    w.additive = vec3(0.0); w.atten = vec3(1.0); w.done = false;
    const bool under = eye.z < waterHeight();
    const float time = time_s * 0.5;
    const vec3  up   = vec3(0.0, 0.0, 1.0);
    const vec3  view = P - eye;
    const float dist = length(view);
    const vec3  viewVec = view / max(dist, 1e-4);

    vec2 wave1d = ssr.water[0].xy, wave2d = ssr.water[0].zw;
    vec3 v = P;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 bigWave = v.xy * vec2(0.04, 0.04) + wave1d * time * 0.055;
    vec4 littleWave;
    littleWave.xy = v.xy * vec2(0.45, 0.9) + wave2d * time * 0.13;
    littleWave.zw = v.xy * vec2(0.1, 0.2)  + wave1d * time * 0.1;
    const int ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    const float blend = ssr.water[1].w;
    const float l1a = rtWaterLod(ia, 0.04), l1b = rtWaterLod(ib, 0.04);
    const float l2a = rtWaterLod(ia, 0.9),  l2b = rtWaterLod(ib, 0.9);
    const float l3a = rtWaterLod(ia, 0.2),  l3b = rtWaterLod(ib, 0.2);
    const vec3 wave1 = mix(rtWaterNormalTex(ia, bigWave, l1a),       rtWaterNormalTex(ib, bigWave, l1b),       blend);
    const vec3 wave2 = mix(rtWaterNormalTex(ia, littleWave.xy, l2a), rtWaterNormalTex(ib, littleWave.xy, l2b), blend);
    const vec3 wave3 = mix(rtWaterNormalTex(ia, littleWave.zw, l3a), rtWaterNormalTex(ib, littleWave.zw, l3b), blend);
    const vec3 wavef = (wave1 + wave2 * 0.4 + wave3 * 0.6) * 0.5;

    if (under)
    {
        w.color = max(applyWaterFog(atmosSrgbToLinear(ssr.water[4].rgb), view, eye.z - waterHeight()), vec3(0.0));
        w.done = true;
        return w;
    }

    const float fscale = ssr.water[2].x, foffset = ssr.water[2].y;
    vec3 df3 = max(vec3(0.0), vec3(dot(viewVec, wave1),
                                   dot(viewVec, (wave2 + wave3) * 0.5),
                                   dot(viewVec, wave3)) * fscale + foffset);
    df3 *= df3;
    const vec2 df2 = max(vec2(0.0), vec2(df3.x + df3.y + df3.z, dot(viewVec, wavef) * fscale + foffset));

    vec3 sunlit, amblit, additive, atten;
    atmosLightTerms(view, normalize(wavef), sunlit, amblit, additive, atten);

    const float vdu = clamp(-dot(viewVec, up) * 2.0, 0.0, 1.0);
    vec3 wave_ibl = wavef * ssr.water[1].xyz;
    wave_ibl.z *= 2.0;
    wave_ibl = normalize(wave_ibl);

    const vec3 fb = applyWaterFog(vec3(1.0), viewVec * 2048.0, eye.z - waterHeight());
    const float rough = clamp(ssr.water[2].z, 0.0, 1.0);
    vec3 R = reflect(viewVec, wave_ibl);
    R.z = abs(R.z);
#ifdef RT_SHADE_ENV
    // (#859 main view: the probes around the water hit along R, as the main view's water FS)
    vec3 radiance;
    {
        vec3 wirr, wrad2;
        RT_SHADE_ENV(P, up, R, rough, false, amblit, wirr, radiance, wrad2);
    }
#else
    vec3 radiance = skyColor(R);   // captures never sample the probes
#endif

    const vec3 specularColor = atmosSrgbToLinear(ssr.water[3].rgb);
    const vec3 V = -viewVec;
    const vec3 glintN = normalize(wavef + up * max(dist, 32.0) / 32.0 * (1.0 - vdu));
    // (no screen derivatives here: the glint keeps the plain roughness -- the specular-AA
    // widening is a main-view anti-sparkle measure)
    float nl; vec3 diffPunc, specPunc;
    llPunctual(vec3(0.0), specularColor, rough, glintN, V, atmosLightDir(), nl, diffPunc, specPunc);
    w.punctual = clamp(nl * (diffPunc + specPunc), vec3(0.0), vec3(10.0)) * sunlit * atten.r;
    w.radiance = radiance * df2.y;
    w.fb = fb;
    w.mixK = min(1.0, df2.x);
    w.additive = additive;
    w.atten = atten;
    return w;
}
// The glint's sun visibility: one ray (the capture water takes the cascades, or nothing with the
// ray-traced sun) -- traced by rtShadeHit (up, no lift) where the glint is above 1e-4.
bool rtShadeWaterSunWanted(RtWaterShade w)
{
    return !w.done && max(max(w.punctual.r, w.punctual.g), w.punctual.b) > 1e-4;
}
vec3 rtShadeWaterEnd(RtWaterShade w, float sunVis)
{
    if (w.done) return w.color;
    vec3 color = mix(w.fb, w.radiance, w.mixK) + w.punctual * sunVis;
    color = atmosApplyRay(color, w.additive, w.atten);   // (#816: the light terms' haze, same ray view = P - eye)
    return max(color, vec3(0.0));
}

// Shade a hit seen from `eye` (the probe origin). list = the capture's light list,
// time_s = the draw time (texture animation / water waves). Returns linear HDR colour +
// the surface alpha (alpha-blend layers composite with it).
// The last rtShadeHit's light split (RenderVKRTReflectionsHitTermsDebug): x the ambient / irradiance
// term, y the environment (specular / shiny) term, z the local lights + emission, w the haze in-scatter
// (luminance each; 0 for water / fullbright).
vec4 g_rts_terms = vec4(0.0);
#ifndef RT_SHADE_HAZE_K
#define RT_SHADE_HAZE_K 1.0
#endif
float rtsLum(vec3 c) { return max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0); }
vec4 rtShadeHit(RtSurface s, vec3 eye, uint list, float time_s)
{
    g_rts_terms = vec4(0.0);
    const vec3 P = s.P;
    const bool water = s.model == RT_SURF_WATER;
    RtWaterShade ws;
    if (water) ws = rtShadeWaterBegin(P, eye, time_s);

    const vec3 V = normalize(eye - P);
    if (!water && s.model == RT_SURF_FULLBRIGHT)
    {
        vec3 fb = s.fb;
        if (s.fb_env > 0.0)
        {
            // Fullbright + shiny: the sharp env (sky in captures) at the shininess level.
            const vec3 fR = reflect(-V, s.Nv);
            const vec3 env = skyColor(fR);
            float fresnel = 1.0 + dot(-V, s.Nv);
            fresnel *= fresnel;
            fresnel = min(fresnel + s.fb_env, 1.0);
            fb = mix(fb, env * (s.fb_env * fresnel) * 0.5, s.fb_env);
        }
        // (clean_draw.frag main(): fullbright takes the haze / water fog afterwards)
#ifndef RT_SHADE_NO_HAZE
        fb = waterFogged(P) ? applyWaterFog(fb, P - eye, eye.z - waterHeight()) : atmosApply(fb, P, eye);
#endif
        return vec4(fb, s.base.a);
    }

    const vec3 N = s.N;
    const bool classic = atmosClassic();
    const vec3 L = atmosLightDir();
    const vec3 rel = P - eye;
    const float dist = length(rel);
    const float foot = rt_surf_foot;
    // Sun / moon visibility, ONCE for every branch below -- the water's glint too (up, no lift):
    // each call site inlines the whole sun walk (opaque / translucent / foliage rays).
    vec3 sunTint = vec3(1.0);
    float sunVis = 1.0;
    if (water ? rtShadeWaterSunWanted(ws) : dot(N, L) > 0.0)
    {
        sunVis = rtHitSunShadow(P, water ? vec3(0.0, 0.0, 1.0) : s.Ng, L, dist, foot, water ? vec3(0.0) : s.tlift, sunTint);
#ifndef RT_SHADE_NO_HAZE
        // (the reflections' grazing fade: a terminator-band hit counts as lit -- a GI hit never: in a
        //  sealed room that fade let the sun in through every smooth wall it grazed)
        if (!water)
            sunVis = mix(1.0, sunVis, max(rtTerminatorFadeT(dot(s.Nv, L), s.tw), smoothstep(0.1, 0.2, dot(s.Ng, L))));
#endif
    }
    if (water) return vec4(rtShadeWaterEnd(ws, sunVis), 1.0);

    // The model's own terms below; the LOCAL LIGHTS and the haze after them, once for both models (one
    // inlined light walk: the grid / cluster loop, its BRDFs and the hit shadow-ray budget's rays).
    const bool pbr = s.model == RT_SURF_PBR;
    vec3 color, additive, atten;
    LegacySpec ls = legacySpecNone();
    if (pbr)
    {
        vec3 sunlit, amblit;
        atmosLightTerms(rel, N, sunlit, amblit, additive, atten);
        if (classic) sunlit *= 1.35;
        const vec3 R = reflect(-V, N);
#ifdef RT_SHADE_ENV
        vec3 irradiance, radiance, radiance2;
        RT_SHADE_ENV(P, N, R, s.rough, false, amblit, irradiance, radiance, radiance2);
#else
        const vec3 irradiance = amblit;
        const vec3 radiance = skyColor(R);
#endif
        const vec3 diffuseColor  = s.base.rgb * (vec3(1.0) - vec3(0.04)) * (1.0 - s.metal);
        const vec3 specularColor = mix(vec3(0.04), s.base.rgb, s.metal);
        const float NdotV = clamp(abs(dot(N, V)), 0.001, 1.0);
        const vec3 iblDiff = irradiance * diffuseColor * s.ao;
        const vec3 iblSpec = radiance * EnvBRDFApprox(specularColor, s.rough, NdotV) * s.ao;
        float nl; vec3 diffPunc, specPunc;
        llPunctual(diffuseColor, specularColor, s.rough, N, V, L, nl, diffPunc, specPunc);
        vec3 stint = vec3(1.0);
        float scol = 1.0;
        if (dot(N, L) > 0.0) { scol = sunVis; stint = sunTint; }
        sunlit *= classic ? atmosLinearToSrgb(stint) : stint;
        if (classic)
        {
            const vec3 irr = atmosSrgbToLinear(irradiance * 0.9);
            const float da = pow(nl, 1.2);
            vec3 sun_contrib = vec3(min(da, scol));
            sun_contrib = atmosSrgbToLinear(atmosLinearToSrgb(sun_contrib) * sunlit * 0.7) * PI;
            const vec3 finalAmbient = irr * diffuseColor;
            const vec3 finalSun     = clamp(sun_contrib * ((diffPunc + specPunc) * scol), vec3(0), vec3(10));
            color = atmosSrgbToLinear(atmosLinearToSrgb(finalAmbient) + atmosLinearToSrgb(finalSun) * 1.1);
        }
        else
        {
            color = iblDiff + clamp(nl * (diffPunc + specPunc), vec3(0), vec3(10)) * sunlit * 3.0 * scol;
        }
        color += iblSpec;
        color += s.emissive;
        g_rts_terms.xy = vec2(rtsLum(classic ? atmosSrgbToLinear(irradiance * 0.9) * diffuseColor : iblDiff),
                              rtsLum(iblSpec));
    }
    else
    {
        // ---- LEGACY ----
        vec3 sunlit, amblit;
        atmosLightTerms(rel, N, sunlit, amblit, additive, atten);
        if (classic) sunlit *= 1.35;
#ifdef RT_SHADE_ENV
        const vec3 R = reflect(-V, N);
        vec3 irradiance, envRad, envRad2;
        RT_SHADE_ENV(P, N, R, 1.0 - s.gloss, true, amblit, irradiance, envRad, envRad2);
        const vec3 glossenv  = s.gloss > 0.0 ? clamp(envRad, vec3(0.0), vec3(10.0)) : vec3(0.0);
        const vec3 legacyenv = s.env > 0.0 ? envRad2 : vec3(0.0);
#else
        const vec3 irradiance = amblit;
        const vec3 R = reflect(-V, N);
        const vec3 glossenv  = s.gloss > 0.0 ? clamp(skyColor(R), vec3(0.0), vec3(10.0)) : vec3(0.0);
        const vec3 legacyenv = s.env > 0.0 ? skyColor(R) : vec3(0.0);
#endif
        float da = clamp(dot(N, L), 0.0, 1.0);
        vec3 stint = vec3(1.0);
        float scol = 1.0;
        if (dot(N, L) > 0.0) { scol = sunVis; stint = sunTint; }
        scol = max(scol, s.emask);   // <GMDX> softenLightF: scol = max(shadow, emissive)
        sunlit *= classic ? atmosLinearToSrgb(stint) : stint;
        color = irradiance;
        if (classic)
        {
            da = pow(da, 1.2);
            const vec3 sun_contrib = vec3(min(da, scol));
            color = atmosSrgbToLinear(color * 0.9 + atmosLinearToSrgb(sun_contrib) * sunlit * 0.7);
            sunlit = atmosSrgbToLinear(sunlit);
        }
        else
        {
            color += min(da, scol) * sunlit;
        }
        color *= s.base.rgb;
        if (s.gloss > 0.0)
        {
            {   // sun specular: clean_brdf.glsl legacySpecular (shared with the raster FS)
                float slit;
                const float ss = legacySpecular(N, V, L, s.gloss, slit);
                color += (slit * scol * ss) * sunlit * s.spec_lin;
            }
            vec3 ge = glossenv * 0.5;
            float fresnel = clamp(1.0 + dot(-V, N), 0.3, 1.0);
            fresnel *= fresnel;
            fresnel *= s.gloss;
            ge *= s.spec_lin * fresnel;
            ge *= max(vec3(1.0) - color, vec3(0.0));   // HDR / emissive past 1.0 must not subtract the reflection
            color += ge * 0.5;
        }
        if (s.emask > 0.0) color = mix(color, s.base.rgb, s.emask);   // <GMDX> emissive mask (clean_surface_light.glsl lightSurface)
        if (s.env > 0.0)
        {
            float fresnel = 1.0 + dot(-V, N);
            fresnel *= fresnel;
            fresnel = min(fresnel + s.env, 1.0);
            const vec3 reflected = legacyenv * (s.env * fresnel);
            color = mix(color, reflected * 0.5, s.env);
        }
        ls.spec = s.spec_lin; ls.gloss = s.gloss; ls.env = s.env;
        g_rts_terms.xy = vec2(rtsLum((classic ? atmosSrgbToLinear(irradiance * 0.9) : irradiance) * s.base.rgb),
                              rtsLum(glossenv * 0.25 * s.spec_lin + (s.env > 0.0 ? legacyenv * 0.5 * s.env : vec3(0.0))));
    }
    const vec3 local = rtHitLocalLights(P, N, V, pbr, s.base.rgb, pbr ? s.metal : 0.0, pbr ? s.rough : 1.0, ls, list);
    g_rts_terms.z = rtsLum(pbr ? local + s.emissive : local);
    color = atmosClampHDR(color * (classic ? 1.1 : 1.0)) + local * (classic ? 0.9 : 1.0);
#ifndef RT_SHADE_NO_HAZE
    {
        const float l0 = rtsLum(color);
        color = atmosOrWaterFog(color, P, eye, additive * (RT_SHADE_HAZE_K), atten);
        g_rts_terms.w = max(rtsLum(color) - l0, 0.0);
    }
#endif
    return vec4(color, s.base.a);
}

#endif
