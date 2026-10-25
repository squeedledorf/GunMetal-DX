#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// vkclean — reflection-probe capture sky: the WindLight sky (skyColor) by the
// face ray reconstructed from the face inverse view-projection (origin-relative),
// without sun/moon discs or stars (LL skips them in cube snapshots).
#include "clean_frame_info.glsl"
#define PHYSKY_NOT_IN_PROBES   // (clean_atmos_common.glsl: the probes see the WindLight sky)
#include "clean_atmos_common.glsl"
layout(set = 1, binding = 0) uniform sampler2D uTextures[];   // bindless (cloud noise, rainbow/halo)
#include "clean_clouds.glsl"
#include "clean_vol_cloud_sky.glsl"
#include "clean_sky_bodies.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    mat4 inv_view_proj;   // face clip -> origin-relative world
    vec4 horizon;
    vec4 zenith;
} pc;

void main()
{
    vec2 ndc = v_uv * 2.0 - 1.0;
    vec4 p = pc.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w);
    // LL skips discs/stars in cube snapshots but keeps the rainbow/halo (skyF) and
    // clouds in the radiance pass.
    // (#719: the volumetric layer's panorama when it is live; the 2D dome steps aside then.)
    outColor = vec4(volCloudSkyApply(skyResolve(cloudComposite(skySceneryPre(dir), dir)), dir), 1.0);
}
