// vkclean -- MOTION BLUR (#118, RenderVKMotionBlur; vkgfx/llcleanmotionblurvk.h).
// Shared by clean_mblur_tile.comp, clean_mblur_neighbor.comp, clean_mblur_recon.comp
// and clean_mblur_writeback.frag: one set layout for the whole effect (every pass binds
// the same set and reads what it needs), all images GENERAL except the renderer's.
//
// VELOCITY convention inside the effect: the BLUR RADIUS vector in RENDER pixels -- half
// the screen displacement over the shutter, after the camera / object scales, capped at
// the max radius (McGuire et al. 2012's V). A pixel's blur spans X - V .. X + V.

// C++ LLCleanMotionBlurVK::Params (std140, 256 B), updated once per recording.
layout(std140, set = 0, binding = 0) uniform MbParams
{
    mat4 reproj_cam;   // this frame's UNJITTERED clip -> last frame's clip (camera rotation + translation)
    mat4 reproj_rot;   // the same with last frame's eye moved onto this frame's (rotation / zoom only)
    vec4 wplane;       // the water plane in this frame's unjittered clip space (all 0 = none)
    vec4 lin;          // view z = (x d + y) / (z d + w) of a reversed-Z device depth d
    vec4 scales;       // x camera rotation, y camera translation, z object, w their max
    vec4 p0;           // x shutter scale (frames of motion), y max radius (render px), z taps, w motion target valid
    vec4 p1;           // x noise frame offset (0 = fixed pattern), y debug mode, z render w, w render h
    vec4 p2;           // x tile size (px), y NeighborMax reach (tiles), z tiles x, w tiles y
    vec4 p3;           // x static exposure, y dynamic exposure on, z active-tile threshold (px), w speed-FoV clip-xy scale k (#663; 0 / 1 = none)
    vec4 p4;           // #836: x 1 = uniform fast path checks the whole reach (RenderVKMotionBlurWideUniformCheck),
                       //       y 1 = Karis HDR tap weights (RenderVKMotionBlurHdrWeight), zw unused
} mb;

layout(set = 0, binding = 1)  uniform sampler2D uDepth;     // world depth, reversed-Z (texelFetch)
layout(set = 0, binding = 2)  uniform sampler2D uMotion;    // RG16F pre-pass motion (UV, cur - prev; texelFetch)
layout(set = 0, binding = 3)  uniform sampler2D uReact;     // pass 2 mask: G = water surface (texelFetch)
layout(set = 0, binding = 4)  uniform sampler2D uScene;     // HDR scene (render size, LINEAR clamp)
layout(set = 0, binding = 6)  uniform sampler2D uVel;       // xy radius vector (px), z linear depth (texelFetch)
layout(set = 0, binding = 8)  uniform sampler2D uTileMax;   // per tile: xy max vector, z min length, w max length
layout(set = 0, binding = 10) uniform sampler2D uNeighbor;  // per tile: xy dominant vector, z min length, w active
layout(set = 0, binding = 12) uniform sampler2D uOut;       // the reconstruction (texelFetch)
layout(r32f, set = 0, binding = 13) uniform readonly image2D uExposure;   // 1x1 dynamic exposure

const int   kTile   = 16;       // render px per tile side (= the tile / reconstruction workgroup)
const float kSkyZ   = 60000.0;  // linear depth stored for the sky (fits RGBA16F)

ivec2 renderSize() { return ivec2(mb.p1.zw); }
ivec2 tileCount()  { return ivec2(mb.p2.zw); }

// This frame's exposure (static x dynamic): the scene is linear HDR before it.
float sceneExposure()
{
    float e = mb.p3.x;
    if (mb.p3.y > 0.5) e *= imageLoad(uExposure, ivec2(0)).r;
    return max(e, 1e-4);
}

// Debug views write display-space colours; they go through exposure + tonemap like the
// scene, so divide by this frame's exposure to land near their intended brightness.
float debugExposure() { return sceneExposure(); }

// Hue wheel for a 2D vector (screen right = red, up = yellow-green, left = cyan, down =
// blue-magenta), brightness sqrt(length / full).
vec3 vectorColour(vec2 v, float full)
{
    const float len = length(v);
    if (len < 1e-4) return vec3(0.0);
    const float hue = atan(-v.y, v.x) / 6.28318531 + 1.0;
    const vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return rgb * sqrt(clamp(len / max(full, 1e-3), 0.0, 1.0));
}
