#version 450
// vkclean -- MOTION BLUR (#118), pass 4: WRITE-BACK. A fullscreen draw into the render-
// size HDR scene (its own colour attachment, opaque write) that copies the
// reconstruction over the tiles the reconstruction wrote (NeighborMax w) and discards
// everywhere else: a still frame costs one tile fetch per pixel, not a full-screen copy,
// and the scene target needs no storage usage. Every consumer after it (TAA / FSR, the
// SSR copy, the upscale blit, the post chain) reads the scene as before.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

// Only the two bindings it reads (not clean_mblur_common.glsl: that declares the scene
// this draw renders into, and the compute-only storage images).
layout(set = 0, binding = 10) uniform sampler2D uNeighbor;  // per tile: w = reconstructed
layout(set = 0, binding = 12) uniform sampler2D uOut;       // the reconstruction (texelFetch)
const int kTile = 16;                                       // clean_mblur_common.glsl kTile

void main()
{
    const ivec2 q = ivec2(gl_FragCoord.xy);
    if (texelFetch(uNeighbor, q / kTile, 0).w < 0.5) discard;
    outColor = texelFetch(uOut, q, 0);
}
