// vkclean — per-map UV transforms (LLCleanMaterialStore::MaterialUV, parallel to the
// materials, draw set 0 binding 16). LL samples the normal / metallic-roughness (legacy:
// specular) / emissive maps with their OWN transform (glTF KHR_texture_transform per
// slot; legacy LLMaterial normal/specular repeats, offset, rotation), not the base one.
// Per slot: row0 = (a, b, c, own), row1 = (d, e, f, 0); u' = a u + b v + c,
// v' = d u + e v + f. own == 0: the slot samples at the base-colour UV.
#ifndef CLEAN_MAT_UV_GLSL
#define CLEAN_MAT_UV_GLSL

#define MAP_NORMAL   0
#define MAP_MR       1
#define MAP_EMISSIVE 2

struct MaterialUV { vec4 rows[6]; };
layout(std430, set = 0, binding = 16) readonly buffer MaterialUVs { MaterialUV mat_uv[]; };

bool matMapOwnUV(uint mid, int slot)
{
    return mat_uv[mid].rows[slot * 2].w > 0.5;
}

// The slot's UV from the RAW (post-texgen, pre-transform) texcoord; base_uv when the
// slot has no transform of its own.
vec2 matMapUV(uint mid, int slot, vec2 raw_uv, vec2 base_uv)
{
    vec4 r0 = mat_uv[mid].rows[slot * 2];
    if (r0.w <= 0.5) return base_uv;
    vec4 r1 = mat_uv[mid].rows[slot * 2 + 1];
    return vec2(dot(r0.xyz, vec3(raw_uv, 1.0)), dot(r1.xyz, vec3(raw_uv, 1.0)));
}

// <GMDX> The slot's UV on a texture-animated face (instance flag 1024), anim_uv = the
// animated base UV (cleanFaceUV). glTF: the slot's own KHR transform applies AFTER the
// animation (LL textureUtilV texture_transform: animation first, then KHR), and it is
// affine, so applying it to the interpolated anim_uv is exact. Legacy: LL drops the
// normal / specular maps' own transforms while a texture animation plays (llface.cpp
// getGeometryVolume: "override specular and normal map tex coords with diffuse
// texcoords"), so they sample at anim_uv like the base colour.
vec2 matMapAnimUV(uint mid, int slot, vec2 anim_uv, bool gltf)
{
    return gltf ? matMapUV(mid, slot, anim_uv, anim_uv) : anim_uv;
}
// </GMDX>

// The slot's transform as a uv_matrix-shaped mat4 (column i = LLMatrix4 row i), for
// cleanFaceTangent's inverse-UV tangent frame.
mat4 matMapUVMatrix(uint mid, int slot)
{
    vec4 r0 = mat_uv[mid].rows[slot * 2];
    vec4 r1 = mat_uv[mid].rows[slot * 2 + 1];
    return mat4(vec4(r0.x, r1.x, 0.0, 0.0),
                vec4(r0.y, r1.y, 0.0, 0.0),
                vec4(0.0, 0.0, 1.0, 0.0),
                vec4(r0.z, r1.z, 0.0, 1.0));
}

#endif
