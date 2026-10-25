// vkclean -- VOLUMETRIC CLOUD SHADOW MAP lookup (#719; LLCleanVolCloudsVK, clean_vol_cloud_shadowmap.comp).
//
// The volumetric cloud layer (RenderVKVolumetricClouds 1) keeps a top-down sun / moon
// TRANSMITTANCE map of the cloud shell around the camera. Texel (u, v) holds the
// transmittance of the light ray that crosses the shell's BASE PLANE (z = base altitude) at
// the texel's world xy, integrated from the base up through the top along the light
// direction. A point P below the base receives exactly that ray: project P along the light
// onto the base plane,
//     Pb = P + L * (base - P.z) / L.z,   uv = (Pb.xy - centre) * inv_extent + 0.5,
// so the lookup is exact at every altitude below the layer (ground, roofs, avatars, the air
// the god rays / froxels march through). Inside or above the layer the same projection is an
// approximation (the part of the shell between P and the base is counted too); above the
// top the light no longer crosses the shell: 1.
//
// Multiplicative: callers multiply their sun / moon visibility (shadow maps, RT mask, RT
// rays) by cloudShadowMap(P). 1.0 when the layer is off (ssr.vol_clouds[0].x < 1.5), the
// cloud shadow is off (strength 0), the light is at / below the horizon, or P projects
// outside the map (the edge fades to 1 over the outer 10%, so no hard square).
//
// P is an AGENT-FRAME world position (the frame every lit pass shades in), not camera-relative.
// Requires clean_frame_info.glsl (ssr) and DRAW SET 0 as set 0 (binding 46: the map, or a
// 1x1 white stand-in while the layer is off -- every pipeline on mDslDraw0 may use it).
#ifndef CLEAN_CLOUD_SHADOW_MAP_GLSL
#define CLEAN_CLOUD_SHADOW_MAP_GLSL

// A pass whose set 0 is NOT draw set 0 (e.g. a froxel compute with its own layout) defines
// CLOUD_SHADOW_SET / CLOUD_SHADOW_BINDING before including this and binds the same image
// (LLCleanVolCloudsVK::exportSampler / shadowView, or the stand-in while the layer is off).
#ifndef CLOUD_SHADOW_SET
#define CLOUD_SHADOW_SET 0
#endif
#ifndef CLOUD_SHADOW_BINDING
#define CLOUD_SHADOW_BINDING 46
#endif
layout(set = CLOUD_SHADOW_SET, binding = CLOUD_SHADOW_BINDING) uniform sampler2D uCloudShadowMap;   // r = transmittance

bool cloudShadowOn() { return ssr.vol_clouds[0].x > 1.5 && ssr.vol_clouds[0].w > 0.0; }

float cloudShadowMap(vec3 P)
{
    if (!cloudShadowOn()) return 1.0;
    const vec3 L = ssr.vol_clouds[2].xyz;          // the light the map was built for
    const float base = ssr.vol_clouds[0].y;
    if (L.z <= 0.02 || P.z >= ssr.vol_clouds[0].z) return 1.0;
    const vec2 pb = P.xy + L.xy * ((base - P.z) / L.z);
    const vec2 uv = (pb - ssr.vol_clouds[1].xy) * ssr.vol_clouds[1].z + 0.5;
    const vec2 e = abs(uv - 0.5) * 2.0;            // 0 centre .. 1 edge
    const float edge = 1.0 - smoothstep(0.9, 1.0, max(e.x, e.y));
    if (edge <= 0.0) return 1.0;
    const float t = textureLod(uCloudShadowMap, uv, 0.0).r;
    // Strength (RenderVKVolCloudShadowStrength) and the low-sun fade (the map's texels
    // stretch along the light as L.z -> 0; long grazing shadows fade out instead).
    const float k = ssr.vol_clouds[0].w * edge * smoothstep(0.02, 0.12, L.z);
    return mix(1.0, clamp(t, 0.0, 1.0), k);
}

#endif
