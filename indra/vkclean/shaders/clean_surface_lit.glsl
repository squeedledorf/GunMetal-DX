// vkclean -- the DECODED SURFACE (SURF_* models, SurfaceLit, surfaceInit): split out of
// clean_surface_light.glsl (#970) so the ray-traced hit weather (clean_rt_weather.glsl) can run
// clean_weather_surface.glsl's weatherSurface on a hit without the lit FS's bindings (the RT probe
// capture declares its own set 0 lights / atlas bindings). No bindings, no dependencies.
#ifndef CLEAN_SURFACE_LIT_GLSL
#define CLEAN_SURFACE_LIT_GLSL

const uint SURF_NONE       = 0u;   // G-buffer: no world surface (sky / cleared)
const uint SURF_PBR        = 1u;   // glTF metallic-roughness (incl. PBR terrain)
const uint SURF_LEGACY     = 2u;   // LL legacy Blinn-Phong / plain (incl. legacy terrain)
const uint SURF_FULLBRIGHT = 3u;   // world fullbright (+ fullbright shiny env)

struct SurfaceLit {
    uint  model;      // SURF_*
    vec3  albedo;     // PBR: base colour (linear); legacy: LL base_lin; fullbright: the
                      // linearized colour before the HDR scale
    vec3  N;          // shading normal (normal-mapped, oriented to the visible side)
    vec3  Ng;         // geometric normal, oriented (shadow offsets; fullbright env)
    float rough;      // PBR perceptual roughness
    float metal;      // PBR metallic
    float ao;         // PBR ORM occlusion
    vec3  emissive;   // PBR emissive (linear)
    vec3  spec;       // legacy specular colour (linear)
    float gloss;      // legacy glossiness (specular exponent x normal-map alpha)
    float env;        // legacy env intensity; fullbright: the shiny env level
    float glareA;     // legacy alpha-blend: the face (non-texture) alpha LL's glare
                      // scales -- al = max(texel alpha, glare) * vertex alpha (materialF).
                      // 0 = no glare (opaque, G-buffer, PBR). Not in the G-buffer.
    float emask;      // <GMDX> legacy EMISSIVE-MASK weight (LL getEmissive: the diffuse alpha
                      // under DIFFUSE_ALPHA_MODE_EMISSIVE, else 0): softenLightF's
                      // mix(lit, base, emissive). Deferred: G2.y. 0 = every other face.
    bool  cel;        // CEL SHADING (#550): toon lighting (clean_cel_common.glsl). Forward:
                      // the VS's v_cel bit 16 (scope + per-avatar selection); deferred:
                      // G2.w bit 3. Never set on fullbright / HUD / water.
};

SurfaceLit surfaceInit()
{
    SurfaceLit s;
    s.model = SURF_NONE;
    s.albedo = vec3(0.0); s.N = vec3(0.0, 0.0, 1.0); s.Ng = vec3(0.0, 0.0, 1.0);
    s.rough = 1.0; s.metal = 0.0; s.ao = 1.0; s.emissive = vec3(0.0);
    s.spec = vec3(0.0); s.gloss = 0.0; s.env = 0.0; s.glareA = 0.0;
    s.emask = 0.0;   // <GMDX>
    s.cel = false;
    return s;
}

#endif   // CLEAN_SURFACE_LIT_GLSL
