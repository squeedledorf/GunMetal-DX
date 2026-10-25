// vkclean — GPU particle records (port of the GL clean particle system). Byte layout
// = CleanParticle (208 B) / CleanEmitter (256 B) in newview/llcleanparticlestore.h.
// In Vulkan tex_handle.x carries the bindless texture INDEX (0xFFFFFFFF = not
// resident yet -> the particle is not drawn); .y is unused.
#ifndef CLEAN_PARTICLE_COMMON_GLSL
#define CLEAN_PARTICLE_COMMON_GLSL

struct Particle {
    vec4  pos_age;      // xyz world position, w age
    vec4  vel_maxage;   // xyz velocity, w max age
    vec4  accel;        // xyz acceleration, w bounce plane Z
    vec4  start_color;
    vec4  end_color;
    vec4  scale;        // startScale.xy, endScale.zw
    vec4  glow;         // startGlow, endGlow, z volumetric sprite fade (#721, clean_vol_part.comp; 0 on spawn), 0
    uvec4 misc;         // flags, blend_bucket, emitter_idx, alive
    uvec2 tex_handle;
    uvec2 pad;          // .x ribbon: own serial, else 0; .y source id (emitter misc.w,
                        // LLViewerPartSource mID: the particle pick's owner, #481)
    vec4  prev_pos;     // ribbon: .x / .y = PARENT index / serial (uint bits; the particle
                        // the same source emitted next, LL mParent); non-ribbon: own position
    vec4  axis;         // ribbon: width axis (source +Z at spawn, LL mAxis); non-ribbon: source +Z
    vec4  prev_axis;    // non-ribbon: source +Z last frame (ribbon: unused)
    vec4  pos_offset;   // FOLLOW_SRC offset (.xyz) / SPIRAL phase (.w)
};

struct Emitter {
    vec4  pos;          // xyz source pos, w burst_radius
    vec4  rot;          // emission rotation quaternion
    vec4  accel;        // xyz per-particle accel, w part max age
    vec4  start_color;
    vec4  end_color;
    vec4  scale;        // startScale.xy, endScale.zw
    vec4  glow;         // startGlow, endGlow
    vec4  speed_angle;  // speedMin, speedMax, innerAngle, outerAngle
    vec4  target;       // xyz target pos, w bounce plane Z
    uvec4 misc;         // pattern, flags, spawn_count, rng_seed
    uvec2 tex_handle;
    uvec2 blend;        // blend_bucket, motion mode
    vec4  prev_pos;     // ribbon point P-1
    vec4  prev_rot;     // emission rotation last frame
    vec4  wind;         // region wind at the source (xyz)
    vec4  src_rot;      // source render rotation alone (ribbon width axis, LL mAxis; no omega spin)
    vec4  prev_pos3;    // ribbon point P-3
};

// Procedural motion modes (emitter.blend.y) for non-script sources.
const uint MODE_NORMAL = 0u;
const uint MODE_SPIRAL = 1u; // spiral + chat: orbit the source, rising
const uint MODE_BEAM   = 2u; // pointing/select beam: lerp source -> target

// LL particle flags (LLPartData::LL_PART_*).
const uint F_INTERP_COLOR    = 0x01u;
const uint F_INTERP_SCALE    = 0x02u;
const uint F_BOUNCE          = 0x04u;
const uint F_WIND            = 0x08u;
const uint F_FOLLOW_SRC      = 0x10u;
const uint F_FOLLOW_VELOCITY = 0x20u;
const uint F_TARGET_POS      = 0x40u;
const uint F_TARGET_LIN      = 0x80u;
const uint F_RIBBON          = 0x400u;
// PARTICLE FOG (#1003; not an LL flag -- bit 29 is unused by LL_PART_*): the CPU sets it on a
// converted system's emitter (LLCleanPartFogVK::markConverted), the emit pass copies it into
// new particles and the sim keeps live ones in step with their own emitter. The froxel
// injection (#721, clean_vol_part.comp) skips them: their density lives in the bricks.
const uint F_PFOG            = 0x20000000u;

// ---- Age-interpolated appearance (the draw VS and the per-particle lighting compute) ----
float partAgeT(Particle p)
{
    const float maxage = p.vel_maxage.w;
    return (maxage > 0.0) ? clamp(p.pos_age.w / maxage, 0.0, 1.0) : 0.0;
}
vec2 partScaleAt(Particle p, float t)
{
    return ((p.misc.x & F_INTERP_SCALE) != 0u) ? mix(p.scale.xy, p.scale.zw, t) : p.scale.xy;
}
vec4 partColorAt(Particle p, float t)
{
    return ((p.misc.x & F_INTERP_COLOR) != 0u) ? mix(p.start_color, p.end_color, t) : p.start_color;
}

// ---- PER-PARTICLE LIGHTING record (RenderVKParticleLighting 1, clean_particle_light.comp
//      -> clean_particle.vert -DPART_VERTEX_LIGHT). PART_LIGHT_VEC4 vec4 per pool slot:
//   [0] rgb  sun + ambient factor (linear; x the classic 1.1), a  haze slope (scalar)
//   [1] rgb  local-light factor (linear; x the classic 0.9),   a  near-camera fade [0, 1]
//   [2] rgb  haze offset (linear)                              a  1 + the ray-traced sun visibility history (#666; 0 = none)
// The particle FS then shades lin = clampHDR([0].rgb * base) + [1].rgb * base, and an
// "over" pair takes lin * [0].a + [2].rgb (the atmosphere / water fog / distance fade
// composite is affine in lin with a scalar slope, so it is exact at the particle centre).
const uint PART_LIGHT_VEC4 = 3u;

// Blend-pair classes (pair = src * 8 + dst, LLCleanParticleStore::BLEND_FACTOR_*) for
// the near-camera fade and the "draws nothing" collapse:
//   1  src SOURCE_ALPHA, dst ONE or ONE_MINUS_SOURCE_ALPHA: alpha 0 leaves every target as is
//   2  src ONE,          dst ONE or ONE_MINUS_SOURCE_ALPHA: rgba 0 leaves every target as is
//   0  anything else (modulate, dest-driven, ZERO dst ...): never faded or collapsed
uint partFadeClass(uint pair)
{
    const uint s = pair >> 3u, d = pair & 7u;
    if (d != 0u && d != 7u) return 0u;
    return s == 6u ? 1u : (s == 0u ? 2u : 0u);
}
vec4 partApplyFade(vec4 col, uint cls, float fade)
{
    if (cls == 1u) col.a *= fade;
    else if (cls == 2u) col *= fade;
    return col;
}
// True when a sprite of this tint can change no target (HDR, glow, reactive): the
// VS collapses it before rasterisation. (The texel is <= 1, so the tint bounds it.)
bool partInvisible(vec4 col, uint cls)
{
    const float eps = 1.0 / 1024.0;
    if (cls == 1u) return col.a < eps;
    if (cls == 2u) return max(max(col.r, col.g), max(col.b, col.a)) < eps;
    return false;
}
// The reduced-resolution particle pass (RenderVKParticleLowRes) composes a pair exactly
// when neither factor reads the destination and the destination factor is a scalar of
// the source alpha (dst' = dst * f + g, f scalar): src not DEST_COLOR / ONE_MINUS_DEST_COLOR,
// dst one of ONE, ZERO, SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA. = LLCleanRendererVK::partLowResPair.
bool partLowResPair(uint pair)
{
    const uint s = pair >> 3u, d = pair & 7u;
    return s != 2u && s != 4u && (d == 0u || d == 1u || d == 6u || d == 7u);
}
// NEAR-CAMERA FADE (standard sprite near fade, by screen footprint): the sprite's
// height as a fraction of the view height, sz / (2 vz tan(fovy/2)); faded out between
// range.x and range.y (range.y <= range.x: off). A camera inside the sprite (vz below
// half its size) is fully past the range.
float partNearFade(float vz, float sz, float tanHalfFovY, vec2 range)
{
    if (range.y <= range.x || range.x <= 0.0) return 1.0;
    const float frac = (vz <= 0.5 * sz) ? 1e6 : sz / (2.0 * vz * max(tanHalfFovY, 1e-4));
    return 1.0 - smoothstep(range.x, range.y, frac);
}

#endif
