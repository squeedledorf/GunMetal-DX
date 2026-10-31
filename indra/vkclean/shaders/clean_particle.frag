#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#extension GL_EXT_ray_query : require   // ray-traced projector shadows (clean_draw_common.glsl)
#endif
// vkclean — GPU particle fragment shader (port of cleanParticleF / cleanParticleGlowF).
// Particles are unlit (fullbright): the texture is resolved UNORM, so texel * colour
// is the GL path's stored sRGB product, linearized here into the linear HDR target
// like the world's fullbright faces. Each SL blend pair is a separate pipeline
// (fixed-function factors); alpha-over pairs also take the haze + far fade, the
// additive / modulate ones stay unhazed (haze added per overlapping sprite would
// accumulate). Glow: the SL START/END_GLOW accumulates additively into the glow
// target, masked by where the sprite is visible (GL's scene-alpha glow pass).
//
// Shares the world FS core (bindless textures, atmosphere) -- the particle VS writes
// the same interface locations 0..5 (v_material_id carries the texture index,
// v_flags the blend bucket).
#define CLEAN_NO_TANGENT_IN   // the particle VS writes no tangent (location 6 = glow)
#define CLEAN_SPEC_SETTINGS   // graphics-setting specialization constants 8-17 (clean_frame_info.glsl)
#include "clean_draw_common.glsl"
#include "clean_particle_common.glsl"   // partFadeClass
#include "clean_vol_apply.glsl"        // volumetric fog consumer (#718): "over" sprites
#include "clean_weather_veil.glsl"     // precipitation veil consumer (#954): "over" sprites

layout(location = 6) flat in float v_glow;
layout(location = 1) out vec4 outGlow;
// REACTIVE MASK (F5; clean_draw.frag outReact): the sprite's coverage into R of render
// pass 2's third attachment (MAX blend). Sprites have no motion vectors.
layout(location = 2) out vec4 outReact;

// Coverage-aware atmospheric source for the two source-driven over buckets.
// Destination-driven/modulate pairs preserve their blend semantics; additive
// emission takes only RGB transmittance in each path below.
CameraTransport particleMediumSegment(vec3 P,vec3 origin)
{
    const vec3 ray=P-origin;
    return probeCapture() || mirrorPass()
        ? cmFinite(origin,ray/max(length(ray),1e-7),0.0,length(ray),0.0)
        : cmAt(P,CLEAN_VIEW);
}
vec3 particleMediumAttenuation(vec3 P)
{
    const vec3 origin=mainReceiverEye(P,pc.u_cam_time.xyz);
    vec3 transmittance=particleMediumSegment(P,origin).T;
    if(origin.z<waterHeight())
    {
        vec3 rel=P-origin;
        if(P.z>waterHeight()) rel*=clamp((waterHeight()-origin.z)/max(rel.z,1e-7),0.0,1.0);
        transmittance*=waterFogLL(rel,origin.z-waterHeight()).a;
    }
    return transmittance;
}
vec3 particleMediumOver(vec3 color,vec3 P,float coverage,bool premultiplied)
{
    const float weight=premultiplied ? coverage : 1.0;
    const vec3 origin=mainReceiverEye(P,pc.u_cam_time.xyz);
    const bool capture=probeCapture() || mirrorPass();
    const CameraTransport transport=particleMediumSegment(P,origin);
    color=color*transport.T+weight*transport.L;
    if(origin.z<waterHeight())
    {
        vec3 rel=P-origin;
        if(P.z>waterHeight()) rel*=clamp((waterHeight()-origin.z)/max(rel.z,1e-7),0.0,1.0);
        const vec4 water=waterFogLL(rel,origin.z-waterHeight());
        color=color*water.a+weight*water.rgb;
    }
    else if(!capture)
    {
        g_cm_sky_prefix=true;g_cm_sky_origin=origin;
        const float keep=atmosDistanceFadeKeep(P,origin,linfo.cam_fwd.xyz,linfo.zinfo.y);
        if(keep<1.0) color=color*keep+weight*(1.0-keep)*atmosFadeTarget(P-origin,origin.z);
    }
    return color;
}

#ifdef PART_VERTEX_LIGHT
// PER-PARTICLE LIGHTING (RenderVKParticleLighting 1, the main view's default): the
// lighting compute's record (clean_particle_light.comp / clean_particle_common.glsl),
// forwarded by the VS. Per fragment only: texel x tint, the record's two multiply-adds,
// the haze affine, the outputs -- no light / probe / shadow / cluster work.
layout(location = 7) in vec3 v_plight;
layout(location = 8) in vec3 v_plocal;
layout(location = 9) in vec4 v_phaze;   // rgb haze offset, a haze slope

void main()
{
    const uint tex = v_material_id;
    if (tex == 0xFFFFFFFFu) discard;   // particle texture not resident yet
    vec4 c = texture(uTextures[nonuniformEXT(tex)], v_uv) * v_tint;
    const uint bucket = v_flags & 0xFFu;
    const uint bsrc = bucket >> 3u, bdst = bucket & 7u;
    // Skip what cannot change a target before any further work / the blend: alpha 0
    // always (as before); below 1/1024 for the alpha-scaled pairs (src SOURCE_ALPHA,
    // dst ONE / ONE_MINUS_SOURCE_ALPHA), whose whole contribution scales by it.
    if (c.a <= 0.0 || (partFadeClass(bucket) == 1u && c.a < (1.0 / 1024.0))) discard;
    const vec3 base_lin = atmosSrgbToLinear(c.rgb);
    vec3 lin = ((v_flags & 0x100u) != 0u)
             ? base_lin * atmosHdrScale()                                     // EMISSIVE: fullbright
             : atmosClampHDR(v_plight * base_lin) + v_plocal * base_lin;      // lit (LL alpha pool)
    const bool over = (bdst == 7u || bdst == 1u) && (bsrc == 0u || bsrc == 3u || bsrc == 6u);
    if (over && !cmOn()) lin = lin * v_phaze.a + v_phaze.rgb;
    // Volumetric fog (#718) over the hazed "over" sprites, main view only (additive /
    // modulate pairs stay unfogged, as they stay unhazed: it would accumulate per sprite).
    if(cmOn())
    {
        if(over && (bsrc==0u || bsrc==6u))
            lin=particleMediumOver(lin,v_world_pos,c.a,bsrc==0u && bdst==7u);
        else if(bdst==0u && (bsrc==0u || bsrc==6u)) lin*=particleMediumAttenuation(v_world_pos);
    }
    if (!cmOn() && over && volOn() && !probeCapture() && !mirrorPass()) lin = volFogApply(lin, v_world_pos);
    if (over && !probeCapture() && !mirrorPass()) lin = weatherVeilApply(lin, v_world_pos);   // (#954)
    outColor = vec4(max(lin, vec3(0.0)), c.a);
    outGlow  = vec4(vec3(v_glow * c.a), 0.0);   // <SS:Skoomastream> glow amount only (as below)
    outReact = vec4(clamp(c.a, 0.0, 1.0), 0.0, 0.0, 0.0);
}
#else
void main()
{
    const uint tex = v_material_id;
    if (tex == 0xFFFFFFFFu) discard;   // particle texture not resident yet
    vec4 c = texture(uTextures[nonuniformEXT(tex)], v_uv) * v_tint;
    if (c.a <= 0.0) discard;

    const uint bucket = v_flags & 0xFFu;
    vec3 lin;
    vec3 hAdd = vec3(0.0), hAtt = vec3(1.0);   // this ray's haze (#816: the lit branch's, reused below)
    bool hOk = false;
    if ((v_flags & 0x100u) != 0u)
    {
        // EMISSIVE particle: fullbright (LLFace::FULLBRIGHT).
        lin = atmosSrgbToLinear(c.rgb) * atmosHdrScale();
    }
    else
    {
        // Non-emissive particles are LIT in LL's alpha pool (alphaF.glsl lit path):
        // normal = -camera at (llvopartgroup: -LLViewerCamera X axis, facing the
        // viewer), irradiance from the probes, sun/moon min(da, shadow), local
        // lights -- dark at night instead of glowing.
        const vec3 N   = -linfo.cam_fwd.xyz;
        const vec3 V   = mainViewDirection(v_world_pos, pc.u_cam_time.xyz);
        const vec3 L   = atmosLightDir();
        const vec3 rel = v_world_pos - pc.u_cam_time.xyz;
        const bool classic = atmosClassic();
        vec3 sunlit, amblit;
        atmosLightTerms(rel, N, sunlit, amblit, hAdd, hAtt);
        hOk = true;
        if (classic) sunlit *= 1.35;
        vec3 irradiance = amblit;
        {
            vec3 prad, pirr;
            if (sampleProbes(v_world_pos, N, reflect(-V, N), 0.0, amblit, prad, pirr) && !classic)
                irradiance = pirr;
        }
        const float scol = sunShadow(v_world_pos, N);
        float da = clamp(dot(N, L), 0.0, 1.0);
        vec3 col = irradiance;
        if (classic)
        {
            da = pow(da, 1.2);
            col = atmosSrgbToLinear(col * 0.9 + atmosLinearToSrgb(vec3(min(da, scol))) * sunlit * 0.7);
        }
        else
        {
            col += min(da, scol) * sunlit;
        }
        const vec3 base_lin = atmosSrgbToLinear(c.rgb);
        col *= base_lin;
        vec3 local = localLights(v_world_pos, N, V, false, base_lin, 0.0, 1.0);
        lin = atmosClampHDR(col * (classic ? 1.1 : 1.0)) + local * (classic ? 0.9 : 1.0);
    }
    // Haze + far fade only for "over"-style pairs (the result replaces the background
    // by coverage: dst = ONE_MINUS_SOURCE_ALPHA / ZERO with a source-driven src);
    // additive / modulate / dest-driven pairs stay unhazed (haze added per overlapping
    // sprite would accumulate). Factor indices: LLCleanParticleStore::BLEND_FACTOR_*.
    const uint bsrc = bucket >> 3u, bdst = bucket & 7u;
    const bool over = (bdst == 7u || bdst == 1u) && (bsrc == 0u || bsrc == 3u || bsrc == 6u);
    if (over)
    {
        if (!cmOn() && waterFogged(v_world_pos))
            lin = applyWaterFog(lin, v_world_pos - pc.u_cam_time.xyz, pc.u_cam_time.z - waterHeight());
        else
        {
            // (#816: one scatter of the ray for the haze and the fade -- the lit branch's
            // when it ran: the same rel)
            if (!cmOn() && !hOk) atmosScatterHaze(v_world_pos - pc.u_cam_time.xyz, hAdd, hAtt);
            if(!cmOn()) lin = atmosApplyRay(lin, hAdd, hAtt);
            if(!cmOn()) lin = atmosDistanceFade(lin, v_world_pos, pc.u_cam_time.xyz, linfo.cam_fwd, linfo.zinfo.y, hAdd, hAtt);
        }
        // Volumetric fog (#718) over the hazed sprite, main view only.
        if(cmOn() && (bsrc==0u || bsrc==6u))
            lin=particleMediumOver(lin,v_world_pos,c.a,bsrc==0u && bdst==7u);
        if (!cmOn() && volOn() && !probeCapture() && !mirrorPass()) lin = volFogApply(lin, v_world_pos);
        if (!probeCapture() && !mirrorPass()) lin = weatherVeilApply(lin, v_world_pos);   // (#954)
    }
    if(cmOn() && !over && bdst==0u && (bsrc==0u || bsrc==6u))
        lin*=particleMediumAttenuation(v_world_pos);
    outColor = vec4(max(lin, vec3(0.0)), c.a);
    // <SS:Skoomastream> Glow AMOUNT only (additive, as LL's emissive pass adds into screen
    // alpha); the extracts multiply by the composited scene colour.
    outGlow  = vec4(vec3(v_glow * c.a), 0.0);
    outReact = vec4(clamp(c.a, 0.0, 1.0), 0.0, 0.0, 0.0);
}
#endif   // PART_VERTEX_LIGHT
