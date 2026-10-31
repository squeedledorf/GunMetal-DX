// GUNMETAL-DX VOLUMETRIC CLOUDS: the module's per-recording parameter block
// (GMDXVolCloudsVK::Params, vkCmdUpdateBuffer before the first pass that reads it).
// Byte layout = GMDXVolCloudsVK::Params (std140: vec4 / mat4 only). Set VC_SET, binding VC_BINDING.
//
// Copyright (C) 2026, squeedledorf (GunMetal-DX). LGPL 2.1, as the viewer.
#ifndef GMDX_CLOUD_UBO_GLSL
#define GMDX_CLOUD_UBO_GLSL
#ifndef VC_SET
#define VC_SET 1
#endif
#ifndef VC_BINDING
#define VC_BINDING 0
#endif
layout(std140, set = VC_SET, binding = VC_BINDING) uniform GmdxCloudParams {
    vec4 shape0;   // x weather tile (m), y 1 / tile, z 1 / base noise period (m), w 1 / detail period (m)
    vec4 shape1;   // xy the 3D noises' wind offset past vc.origin (m), z extinction per m at density 1,
                   // w detail erosion
    vec4 shape2;   // x sky ambient factor, y max distance (m), z albedo, w beer-powder scale
    vec4 shape3;   // x the smallest cloud footprint (m; gmdx_cloud_weather.comp), y the detail curl near
                   // the base (m), zw 0
    vec4 light;    // x light bounced off the ground, y the self-shadow march's length (m), z / w the
                   // moon's forward lobe: g, weight
    vec4 march;    // x max steps (main view), y light steps, z frame index, w 1 = offscreen (snapshot)
    vec4 cam;      // xyz camera world position (agent frame), w time (s)
    vec4 shadow;   // shadow map: xy centre (m), z extent (m), w steps
    vec4 pano;     // x first row, y rows this dispatch, z steps, w history blend (0 = overwrite)
    // Main view (march / temporal / composite):
    mat4 inv_vp;   // UNJITTERED VK clip -> CAMERA-RELATIVE world (this frame)
    mat4 reproj;   // camera-relative world (this frame) -> last frame's unjittered clip, the wind's move in
    vec4 dims;     // render w, h, march w, h
    vec4 pat;      // x pattern (0 all, 1 checkerboard, 2 one per 2x2), y frame phase, z steps, w light steps
    vec4 temporal; // x max history (frames), y 1 = history reset, z clamp widening, w 1 = the camera is in
                   // the layer (the temporal pass keeps the march end, B.w)
    vec4 flash;    // #783 M2 lightning: xyz unit direction to the strike (world), w its radiance (0 = none)
    vec4 origin;   // xy the agent frame's global origin + the wind past the region's own (m, wrapped by
                   // 21 weather tiles), zw the region's own wind (weather tiles, wrapped by 21)
    vec4 bodies;   // sculpted cloud bodies: x shapes loaded (0 = none), y amount (GMDXCloudBodies),
                   // z density, w the share of cells that hold one (at amount 1)
    vec4 wmap;     // world coverage window (Slipstream #1283): xy agent-frame corner (m), z 1 / extent, w extent
    vec4 wnoise;   // xy its advected global corner / macro scale, z 1 / macro scale, w its strength (0 = off)
    vec4 wrot;     // xy the rotated EEP sample's phase (tiles), zw 0
    vec4 debug;    // x Sky Debugger view (0 = none; gmdx_cloud_common.glsl GC_VIEW_*), yzw 0
} vc;
#endif
