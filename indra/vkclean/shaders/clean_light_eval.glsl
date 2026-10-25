// vkclean — local-light evaluation helpers (LL deferredUtil ports) shared by the lit
// FS (clean_draw_common.glsl) and the ray-traced shadow mask (clean_rt_shadow_mask.comp).
// Needs Light (clean_lights_common.glsl) and uTextures declared before inclusion. All
// texture reads use explicit LOD (valid in compute).
float legacyAtten(float d, float falloff)   // deferredUtil calcLegacyDistanceAttenuation
{
    float a = 1.0 - clamp((d + falloff) / (1.0 + falloff), 0.0, 1.0);
    a *= a;
    return a * 2.0;
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
