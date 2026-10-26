// vkclean — local-light evaluation helpers (LL deferredUtil ports) shared by the lit
// FS (clean_draw_common.glsl), the deferred kernels, the RT hit shading, the ray-traced shadow
// mask (clean_rt_shadow_mask.comp) and the fog passes. Needs Light (clean_lights_common.glsl),
// uTextures and the frame info (clean_frame_info.glsl: the cutoff) declared before inclusion. All
// texture reads use explicit LOD (valid in compute).
float legacyAtten(float d, float falloff)   // deferredUtil calcLegacyDistanceAttenuation
{
    float a = 1.0 - clamp((d + falloff) / (1.0 + falloff), 0.0, 1.0);
    a *= a;
    return a * 2.0;
}

// ---- THE LIGHT REACH (#1043): does light record (pr = pos_radius, cf = color_falloff) reach P, and
//      how strongly -- the range, LL's distance attenuation and the cutoff fade below, in that order.
//      ONE predicate for every light walk: the lit FS / deferred lit kernels, the projector sampler,
//      the RT hit shading (reflections, GI, mirror, RT forward, probe RT), the RT shadow mask's
//      projector rays, both fog passes and the GI change test. Before, the cutoff fade was missing
//      from the RT hits, the mask, the fogs and the GI change test: a surface and its reflection /
//      bounce disagreed about a dim light's tail, the mask cast rays the lit pass then multiplied by a
//      fade of 0. (clean_regir.glsl's cell weights keep their own conservative bound: a box, not a P.) ----
// LIT-PASS TRIM (RenderVKLightCutoff -> ssr.lit_dbg.z, linear radiance; 0 = off): a light
// whose attenuated colour at this point is below the cutoff is faded out over [c, 2c] and
// skipped below c -- before its shadow / gobo fetches. Smooth, so no visible edge where
// the tail of a dim / distant light's falloff used to end.
float lightCutoffFade(float atten, vec3 color)
{
    const float c = ssr.lit_dbg.z;
    if (c <= 0.0) return 1.0;
    return smoothstep(c, 2.0 * c, atten * max(max(color.r, color.g), color.b));
}
// The range test alone, on the position (callers that load the rest of the record only for the lights
// in range): lv = the vector to the light, d2 its squared length.
bool lightInRange(vec4 pr, vec3 P, out vec3 lv, out float d2)
{
    lv = pr.xyz - P;
    d2 = dot(lv, lv);
    return d2 < pr.w * pr.w;
}
// ... then the attenuation x the cutoff fade at squared distance d2 (in range): > 0, or 0 = no reach.
float lightReachAt(vec4 pr, vec4 cf, float d2, out float dist)
{
    dist = sqrt(d2);
    if (dist >= pr.w) return 0.0;
    const float atten = legacyAtten(dist / pr.w, cf.w);
    if (atten <= 0.0) return 0.0;
    return atten * lightCutoffFade(atten, cf.rgb);
}
// Both: the light's attenuation x cutoff fade at P (0 = it does not reach P), lv / dist to it.
float lightReach(vec4 pr, vec4 cf, vec3 P, out vec3 lv, out float dist)
{
    float d2;
    dist = 0.0;
    if (!lightInRange(pr, P, lv, d2)) return 0.0;
    return lightReachAt(pr, cf, d2, dist);
}
// A projector's frustum (proj_mat: world -> projector texture space): true inside, tc = the texture
// coordinate there (the gobo's).
bool lightProjFrustum(mat4 proj_mat, vec3 P, out vec2 tc)
{
    vec4 ptc = proj_mat * vec4(P, 1.0);
    tc = vec2(0.0);
    if (ptc.z < 0.0) return false;
    ptc.xyz /= ptc.w;
    tc = ptc.xy;
    return !(ptc.z <= 0.0 || ptc.x <= 0.0 || ptc.x >= 1.0 || ptc.y <= 0.0 || ptc.y >= 1.0);
}

vec4 projSample(Light L, vec2 tc, float lod)
{
    if (L.tex.y == 0u) return vec4(1.0);   // untextured projector: white (LL sWhiteImagep)
    return textureLod(uTextures[nonuniformEXT(L.tex.x)], tc, lod);
}

// deferredUtil getProjectedLightDiffuseColor / getTexture2DLodDiffuse
vec3 projDiffuse(Light L, float l_dist, vec2 tc)
{
    float proj_lod = L.proj_params.z;
    float lod  = clamp((l_dist - L.proj_params.x) / L.proj_params.y, 0.0, 1.0) * proj_lod;
    vec4  ret  = projSample(L, tc, lod);
    vec2  dist = vec2(0.5) - abs(tc - vec2(0.5));
    float det  = min(lod / max(proj_lod * 0.5, 1e-4), 1.0);
    float edge = 0.25 * det;
    if (edge > 0.0) ret *= clamp(min(dist.x, dist.y) / edge, 0.0, 1.0);
    return L.color_falloff.rgb * ret.rgb * ret.a;
}

// deferredUtil getProjectedLightAmbiance / getTexture2DLodAmbient
vec3 projAmbiance(Light L, float amb_da, float atten, float lit, float nl, vec2 tc)
{
    vec4  a  = projSample(L, tc, L.proj_params.z);
    vec2  dd = tc - vec2(0.5);
    a *= clamp((0.25 - dot(dd, dd)) / 0.25, 0.0, 1.0);
    float amb = L.proj_params.w;
    amb_da += amb;
    amb_da += (nl * nl * 0.5 + 0.5) * amb;
    amb_da *= atten;
    amb_da = min(amb_da, 1.0 - lit);
    return amb_da * L.color_falloff.rgb * a.rgb * a.a;
}

// ONE GOBO FETCH (lit-pass trims): the diffuse term's texel and LOD, so the ambiance term
// can reuse the same fetch where it samples the same LOD (past focus + range the diffuse
// LOD reaches the ambiance's max LOD) -- bit-identical to projDiffuse / projAmbiance.
vec3 projDiffuseT(Light L, float l_dist, vec2 tc, out vec4 texel, out float lod)
{
    float proj_lod = L.proj_params.z;
    lod  = clamp((l_dist - L.proj_params.x) / L.proj_params.y, 0.0, 1.0) * proj_lod;
    texel = projSample(L, tc, lod);
    vec4  ret  = texel;
    vec2  dist = vec2(0.5) - abs(tc - vec2(0.5));
    float det  = min(lod / max(proj_lod * 0.5, 1e-4), 1.0);
    float edge = 0.25 * det;
    if (edge > 0.0) ret *= clamp(min(dist.x, dist.y) / edge, 0.0, 1.0);
    return L.color_falloff.rgb * ret.rgb * ret.a;
}
vec3 projAmbianceT(Light L, float amb_da, float atten, float lit, float nl, vec2 tc, vec4 texel, float lod)
{
    vec4  a  = (lod == L.proj_params.z) ? texel : projSample(L, tc, L.proj_params.z);
    vec2  dd = tc - vec2(0.5);
    a *= clamp((0.25 - dot(dd, dd)) / 0.25, 0.0, 1.0);
    float amb = L.proj_params.w;
    amb_da += amb;
    amb_da += (nl * nl * 0.5 + 0.5) * amb;
    amb_da *= atten;
    amb_da = min(amb_da, 1.0 - lit);
    return amb_da * L.color_falloff.rgb * a.rgb * a.a;
}

// The AMBIANCE's light at tc, N.L-free (an upper bound: amb_da <= 3 x ambiance before the
// attenuation; lit 0): the weight / coverage test of a shadowed projector whose ambiance reaches
// a pixel its direct texel does not (RenderVKProjAmbianceShadowedMode: the RT shadow mask's list
// and REST weight). 0 without ambiance.
vec3 projAmbianceW(Light L, float atten, vec2 tc)
{
    if (!(L.proj_params.w > 0.0)) return vec3(0.0);
    vec4  a  = projSample(L, tc, L.proj_params.z);
    vec2  dd = tc - vec2(0.5);
    a *= clamp((0.25 - dot(dd, dd)) / 0.25, 0.0, 1.0);
    return min(3.0 * L.proj_params.w * atten, 1.0) * L.color_falloff.rgb * a.rgb * a.a;
}

// ---- Legacy projector specular reflection (spotLightF, envIntensity > 0): the shiny
// surface mirrors the projector's image plane. deferredUtil texture2DLodSpecular,
// including LL's extra edge factor (d *= min(1, d * (proj_lod - lod))).
vec3 projSpecularTex(Light L, vec2 tc, float lod)
{
    vec4  ret  = projSample(L, tc, lod);
    float proj_lod = L.proj_params.z;
    vec2  dist = vec2(0.5) - abs(tc - vec2(0.5));
    float det  = min(lod / max(proj_lod * 0.5, 1e-4), 1.0);
    float d    = min(dist.x, dist.y);
    d *= min(1.0, d * (proj_lod - lod));
    float edge = 0.25 * det;
    if (edge > 0.0) ret.rgb *= clamp(d / edge, 0.0, 1.0);
    return ret.rgb;
}

// The projector image seen in the mirror direction at P (V = towards the eye), blurred by
// (1 - gloss): LL projects the reflected ray onto the plane just past the near clip
// (proj_p = apex + axis * (near + 0.01), proj_n = axis; near = L.proj_axis.w, set by the
// light gather) and looks the hit up through proj_mat. Returns linear light colour x
// image texel; the caller applies visibility and env intensity (LL: color * tex *
// shadow * envIntensity -- no distance attenuation).
vec3 projEnvReflect(Light L, vec3 P, vec3 N, vec3 V, float gloss)
{
    const vec3  ref = reflect(-V, N);
    const vec3  pn  = L.proj_axis.xyz;
    const float ds  = dot(ref, pn);
    if (ds >= 0.0) return vec3(0.0);
    const vec3  pp  = L.proj_origin.xyz + pn * (L.proj_axis.w + 0.01);
    const vec3  pf  = P + ref * (dot(pp - P, pn) / ds);
    vec4 stc = L.proj_mat * vec4(pf, 1.0);
    if (stc.z <= 0.0) return vec3(0.0);
    stc /= stc.w;
    if (stc.x >= 1.0 || stc.y >= 1.0 || stc.x <= 0.0 || stc.y <= 0.0) return vec3(0.0);
    return L.color_falloff.rgb * projSpecularTex(L, stc.xy, (1.0 - gloss) * (L.proj_params.z * 0.6));
}
