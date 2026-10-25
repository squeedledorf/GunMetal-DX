// vkclean — CEL SHADING + AVATAR MASK shared state (#549 / #550; llcelshadevk.h).
//
// Draw set 0 binding 37: the cel parameter block (LLCelShadeVK::Params, std430, 192 B at 1024 slots),
// written by the CPU only when it changes (vkCmdUpdateBuffer at the top of recordWorld).
// Read by the world VS (avatar slot + per-avatar selection -> the v_cel varying), the lit
// FS / deferred lighting (toon ramp parameters) and the outline pass.
//
// Draw set 0 binding 38 (VERTEX only, CEL_VERTEX): the skin meta words
// (LLCleanSkinStore::SkinMeta, 1892 words each, word 0 = avatar_idx), so a rigged draw's
// avatar slot comes from its skin_id. Aliased to binding 37's buffer while no skin meta
// exists (cel_flags & CEL_META clear: never read then).
//
// AVATAR SLOT INDEX SPACE: SkinMeta.avatar_idx (rigged mesh + the system avatar body,
// LLCleanSkinStore::assignAvatarSlot) and a worn rigid attachment's skin_id
// (ensureAvatarSlot -> the same assignAvatarSlot) are the SAME joint-table slot of the
// same avatar. Flexi chains (instance bit 19) sit in pseudo slots: never an avatar.
//
// v_cel (VS -> FS, location 11, flat): bits 0..15 = avatar slot + 1 (0 = not an avatar),
// bit 16 = toon lighting applies to this instance (scope + selection resolved in the VS,
// so forward, G-buffer and the id pass always agree).

#ifndef CLEAN_CEL_COMMON_GLSL
#define CLEAN_CEL_COMMON_GLSL
#include "clean_av_slots.glsl"

const uint CEL_LIGHT = 1u;    // toon lighting on (some scope)
const uint CEL_IDS   = 2u;    // avatar ids wanted (the id pass / FX avatar mask)
const uint CEL_ALL   = 4u;    // every avatar counts as selected
const uint CEL_SCENE = 8u;    // whole-scene mode: toon lighting on every lit surface
const uint CEL_META  = 16u;   // binding 38 holds the skin meta
const uint CEL_SPEC  = 32u;   // specular step on

const uint CEL_V_LIT = 65536u;   // v_cel bit 16

layout(std430, set = 0, binding = 37) readonly buffer CelParamsSSBO {
    uint  cel_flags;
    uint  cel_bands;       // lit levels of the N.L ramp (>= 1)
    float cel_softness;    // edge width of a band step, fraction of a band (0 hard .. 1)
    float cel_flatten;     // ambient flattening (0 = as lit .. 1 = constant luminance)
    vec4  cel_shadow;      // rgb shadow-side tint, a = its strength (fill of the dark bands)
    vec4  cel_rim;         // rgb rim colour x strength, a = rim width (0..1 of the grazing range)
    vec4  cel_spec;        // x threshold, y edge softness, z strength, w unused
    uint  cel_sel[AV_SLOT_WORDS];   // per-avatar-slot selection bitset (AV_SLOT_CAP slots)
};

bool celSlotSelected(uint slot)
{
    if ((cel_flags & CEL_ALL) != 0u) return true;
    if (slot >= AV_SLOT_CAP) return false;
    return ((cel_sel[slot >> 5u] >> (slot & 31u)) & 1u) != 0u;
}

#ifdef CEL_VERTEX
layout(std430, set = 0, binding = 38) readonly buffer CelSkinMetaSSBO { uint cel_skin_meta[]; };
const uint CEL_SKIN_META_WORDS = 1892u;   // LLCleanSkinStore::SkinMeta / 4

// The v_cel word of one instance (see the header). rigged: this draw comes through the
// rigged VS (pre-skinned; slot from the skin meta); attached: a worn rigid attachment whose
// skin_id passed the static VS's joint-table bound check (slot = skin_id).
uint celVertexWord(uint flags, uint skin_id, bool rigged, bool attached)
{
    if ((cel_flags & (CEL_LIGHT | CEL_IDS)) == 0u) return 0u;
    if ((flags & (8u | 2048u)) != 0u) return 0u;               // HUD, water
    uint slot = 0xFFFFFFFFu;
    if (rigged)
    {
        if ((flags & 524288u) == 0u && (cel_flags & CEL_META) != 0u && skin_id != 0xFFFFFFFFu)
        {
            const uint w = skin_id * CEL_SKIN_META_WORDS;
            if (skin_id < 0x100000u && w < uint(cel_skin_meta.length())) slot = cel_skin_meta[w];
        }
    }
    else if (attached)
    {
        slot = skin_id;
    }
    if (slot >= 0xFFFFu) slot = 0xFFFFFFFFu;
    uint v = slot != 0xFFFFFFFFu ? slot + 1u : 0u;
    const bool lit = (cel_flags & CEL_LIGHT) != 0u
                  && ((cel_flags & CEL_SCENE) != 0u || (slot != 0xFFFFFFFFu && celSlotSelected(slot)));
    return v | (lit ? CEL_V_LIT : 0u);
}
#endif   // CEL_VERTEX

// ---- toon lighting helpers (clean_surface_light.glsl / clean_draw_common.glsl) ----

// Quantized light amount: x in [0, 1] -> one of cel_bands + 1 levels {0, 1/N, .., 1},
// rounding at the half-band, with a smooth step of cel_softness (fraction of a band).
float celRamp(float x)
{
    const float n = float(max(cel_bands, 1u));
    const float f = clamp(x, 0.0, 1.0) * n;
    const float w = clamp(cel_softness, 0.0, 1.0) * 0.5 + 1e-3;
    const float i = floor(f);
    return clamp((i + smoothstep(0.5 - w, 0.5 + w, f - i)) / n, 0.0, 1.0);
}

float celLuma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Flattened ambient: keep the hue of irr, pull its luminance toward ref's (the sky
// ambient) so the normal-dependent shading of the ambient term mostly disappears.
vec3 celFlatten(vec3 irr, vec3 ref)
{
    const float li = celLuma(irr);
    const float lr = celLuma(ref);
    return irr * (mix(li, lr, clamp(cel_flatten, 0.0, 1.0)) / max(li, 1e-5));
}

// Specular step: a hard-edged highlight of the specular term's luminance.
vec3 celSpecStep(vec3 spec, vec3 tint)
{
    if ((cel_flags & CEL_SPEC) == 0u) return vec3(0.0);
    const float l = celLuma(spec);
    const float t = cel_spec.x, s = max(cel_spec.y, 1e-3) * 0.5;
    return tint * (smoothstep(t - s, t + s, l) * cel_spec.z);
}

// Rim light mask from N.V (0 facing the camera .. 1 at grazing), for silhouettes.
// Surfaces facing up or down (ground, floors, roofs, ceilings) get no rim: seen from a
// low camera they are grazing everywhere, and the rim washed whole landscapes white
// with distance (#726). Nz = the world (agent space, z up) normal's z.
float celRimMask(float NdotV, float Nz)
{
    const float g = 1.0 - clamp(NdotV, 0.0, 1.0);
    const float w = clamp(cel_rim.a, 0.0, 1.0);
    const float s = max(cel_softness, 0.05) * 0.25;
    const float upright = 1.0 - smoothstep(0.35, 0.75, abs(Nz));   // walls 1 .. ground / ceilings 0
    return smoothstep(1.0 - w - s, 1.0 - w + s, g) * upright;
}

#endif   // CLEAN_CEL_COMMON_GLSL
