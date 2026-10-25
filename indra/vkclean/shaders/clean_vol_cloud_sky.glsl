// vkclean -- VOLUMETRIC CLOUD SKY PANORAMA lookup (#719; LLCleanVolCloudsVK, clean_vol_cloud_pano.comp).
//
// While the volumetric cloud layer is live (ssr.vol_clouds[0].x == 2) the classic 2D cloud
// dome (clean_clouds.glsl cloudComposite) is off everywhere, and the views that cannot afford
// the full-resolution march -- reflection-probe captures (raster + ray traced), the planar
// mirror's sky (water / mirror reflections) -- see the clouds through this panorama instead:
// an equirectangular map of the layer as seen from the main camera (rgb = the clouds'
// in-scatter with the aerial perspective already applied, LINEAR HDR premultiplied; a = the
// transmittance), refreshed a band per frame by the clouds module. The main view composites
// its own march (clean_vol_cloud_composite.frag) and skips this.
//
// Parallax: the panorama is from the main eye; probes / mirrors sit within a few hundred
// metres of it while the layer is kilometres away -- no visible shift.
//
// Requires clean_frame_info.glsl (ssr) and DRAW SET 0 as set 0 (binding 47: the panorama or
// a 1x1 stand-in, only read while ssr.vol_clouds[1].w says the panorama is valid).
#ifndef CLEAN_VOL_CLOUD_SKY_GLSL
#define CLEAN_VOL_CLOUD_SKY_GLSL

layout(set = 0, binding = 47) uniform sampler2D uCloudPano;   // rgb in-scatter, a transmittance

bool volCloudsLive() { return ssr.vol_clouds[0].x > 1.5; }

// World direction (Z up) -> panorama uv: u azimuth (U repeats), v = 0 zenith .. 1 nadir.
vec2 volCloudPanoUV(vec3 d)
{
    return vec2(atan(d.y, d.x) * (0.5 / 3.14159265) + 0.5, acos(clamp(d.z, -1.0, 1.0)) * (1.0 / 3.14159265));
}

// lin = the LINEAR HDR sky behind the clouds in direction dir (world).
vec3 volCloudSkyApply(vec3 lin, vec3 dir)
{
    if (!volCloudsLive() || ssr.vol_clouds[1].w < 0.5) return lin;
    const vec4 c = textureLod(uCloudPano, volCloudPanoUV(normalize(dir)), 0.0);
    return lin * clamp(c.a, 0.0, 1.0) + max(c.rgb, vec3(0.0));
}

#endif
