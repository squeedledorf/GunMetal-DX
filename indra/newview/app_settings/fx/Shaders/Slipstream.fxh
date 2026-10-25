/*
 * Slipstream.fxh -- Slipstream's ReShade FX extensions (#548). See ../README.md.
 *
 * #include "Slipstream.fxh" after "ReShade.fxh". Everything here is optional: without
 * Slipstream (__SLIPSTREAM__ undefined) the textures bind to nothing useful, so guard
 * your use with #if __SLIPSTREAM__.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#ifndef __SLIPSTREAM__
	#define __SLIPSTREAM__ 0
#endif

namespace Slipstream
{
	// View-space normal, display resolution, unjittered. xyz = unit normal (camera looks
	// down -z, +y up), w = 1 from the depth pre-pass, 0.5 reconstructed from depth.
	texture NormalTex : SLIPSTREAM_NORMAL;
	// Motion vectors in UV units, current - previous position, unjittered.
	texture MotionTex : SLIPSTREAM_MOTION;
	// Avatar id per pixel (avatar slot + 1, 0 = no avatar), stored as a float.
	texture AvatarMaskTex : SLIPSTREAM_AVATAR_MASK;
	// The linear HDR scene x exposure (values above 1 are real highlights).
	texture SceneHdrTex : SLIPSTREAM_SCENE_HDR;

	sampler Normal { Texture = NormalTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	sampler Motion { Texture = MotionTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	sampler AvatarMask { Texture = AvatarMaskTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
	sampler SceneHdr { Texture = SceneHdrTex; };

	float3 GetNormal(float2 texcoord) { return tex2Dlod(Normal, float4(texcoord, 0, 0)).xyz; }
	float2 GetMotion(float2 texcoord) { return tex2Dlod(Motion, float4(texcoord, 0, 0)).xy; }
	// Avatar id at this pixel (0 = none).
	float GetAvatarId(float2 texcoord) { return tex2Dlod(AvatarMask, float4(texcoord, 0, 0)).x; }
	bool IsAvatar(float2 texcoord) { return GetAvatarId(texcoord) > 0.5; }
	float3 GetSceneHdr(float2 texcoord) { return tex2Dlod(SceneHdr, float4(texcoord, 0, 0)).rgb; }
}
