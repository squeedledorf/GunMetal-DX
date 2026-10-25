#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — GPU particle billboard / ribbon vertex shader (port of cleanParticleV.glsl).
// Instanced: one instance per live particle in the blend-pair segment being drawn (the
// sorted draw list holds their pool indices back to front); 6 vertices each. Color / scale /
// glow are interpolated from age here. Outputs match clean_draw_common.glsl's inputs
// (locations 0..5) so the particle FS can share the world FS core (haze, bindless).
#include "clean_particle_common.glsl"

layout(std430, set = 2, binding = 0) readonly buffer Particles { Particle parts[]; };
layout(std430, set = 2, binding = 1) readonly buffer Emitters  { Emitter emit[]; };
layout(std430, set = 2, binding = 2) readonly buffer DrawList  { uvec2 draw_list[]; };   // (sort key, pool index), every pass's list

// The draw view (VKDrawPush, #238: a ring entry + 16 B push). The particle draw reuses
// two slots the world VS does not read here: the inv_view_proj slot = world->view
// (billboard basis), the water_fog slot = .x blend pair, .y pool size (ribbon parent bound),
// .z / .w the size routing (#879, below).
#include "clean_draw_view.glsl"

layout(location = 0) flat out uint  v_tex;          // bindless index (0xFFFFFFFF = not resident)
layout(location = 1)      out vec2  v_uv;
layout(location = 2)      out vec3  v_world_normal; // unused (unlit), fixed +Z
layout(location = 3) flat out vec4  v_tint;         // age-interpolated colour (sRGB)
layout(location = 4) flat out uint  v_flags;        // blend pair | 0x100 emissive
layout(location = 5)      out vec3  v_world_pos;
layout(location = 6) flat out float v_glow;         // age-interpolated SL glow

#ifdef PART_VERTEX_LIGHT
// PER-PARTICLE LIGHTING (RenderVKParticleLighting 1; clean_particle_light.comp): the
// record of this vertex's particle (a ribbon's parent end: the parent's), forwarded to
// the FS, which no longer lights anything itself. Main view only (the mirror capture and
// the shadow casters keep the plain VS). The record's near-camera fade scales the tint.
layout(std430, set = 2, binding = 3) readonly buffer PartLight { vec4 plight[]; };
layout(location = 7) out vec3 v_plight;   // sun + ambient factor
layout(location = 8) out vec3 v_plocal;   // local-light factor
layout(location = 9) out vec4 v_phaze;    // rgb haze offset, a haze slope
// VOLUMETRIC PARTICLES (#721, clean_vol_part.comp): the frame info's vol_eye.w > 1.5 = the
// fog's bin pass ran this frame with sprite replacement; Particle.glow.z then holds the
// fraction of this sprite's coverage that moved into the froxel volume (main view only:
// the mirror / shadow passes keep the plain VS and the whole sprite).
#include "clean_frame_info.glsl"
#endif

// Nothing to draw for this particle: all 6 vertices outside the clip volume.
void collapseParticle(uint bucket, vec3 P)
{
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    v_uv = vec2(0.0);
    v_tint = vec4(0.0);
    v_glow = 0.0;
    v_tex = 0xFFFFFFFFu;
    v_flags = bucket;
    v_world_normal = vec3(0.0, 0.0, 1.0);
    v_world_pos = P;
#ifdef PART_VERTEX_LIGHT
    v_plight = vec3(0.0); v_plocal = vec3(0.0); v_phaze = vec4(0.0, 0.0, 0.0, 1.0);
#endif
}

void main()
{
    // Triangle list (6 verts): (0,1,2) (2,1,3) over the strip corners 0..3.
    const int CORNER[6] = int[6](0, 1, 2, 2, 1, 3);
    const int corner = CORNER[gl_VertexIndex % 6];
    // The pass's draw list (#518: main / mirror sorted, cascade unsorted) holds this
    // segment's live particles that pass sees, back to front; its indirect draw starts
    // at the segment's offset (firstInstance, included in gl_InstanceIndex) and covers
    // exactly them -- no per-instance discard.
    const uint bucket = uint(pc.u_water_fog.x + 0.5);   // blend pair (FS: haze rule)
    const uint maxp   = uint(pc.u_water_fog.y + 0.5);
    uint idx = draw_list[uint(gl_InstanceIndex)].y;
    Particle p = parts[idx];

    float t = partAgeT(p);

    uint flags = p.misc.x;
    vec2 scl = partScaleAt(p, t);
    // SIZE ROUTING (#879; RenderVKParticleSmallPx): the draw's u_water_fog.z = 0 draws
    // every sprite; > 0 only the LARGE ones (projected size >= z px: the reduced-resolution
    // pass / the coarse shading rate), < 0 only the SMALL ones (< |z| px: full resolution,
    // full shading rate -- a few-pixel sprite at half resolution or 2x2 shading is a
    // blocky, one-mip-blurrier smear). u_water_fog.w = render px per metre at view depth 1
    // (rh / (2 tan(fovy / 2))). Decided per particle at its centre (every vertex agrees);
    // ribbons (thin strips, no footprint) count as small; behind the eye = large.
    {
        const float route = pc.u_water_fog.z;
        if (route != 0.0)
        {
            bool isSmall = (flags & F_RIBBON) != 0u;
            if (!isSmall)
            {
                const float cw = (pc.u_view_proj * vec4(p.pos_age.xyz - pc.u_origin.xyz, 1.0)).w;
                isSmall = cw > 1e-4 && max(scl.x, scl.y) * pc.u_water_fog.w < abs(route) * cw;
            }
            if (isSmall != (route < 0.0)) { collapseParticle(bucket, p.pos_age.xyz); return; }
        }
    }
    vec4 col = partColorAt(p, t);
#ifdef PART_VERTEX_LIGHT
    // Near-camera fade (per particle, from the lighting compute), then the collapse of a
    // sprite that cannot change any target (fully transparent / faded): no raster at all.
    {
        const uint fcls = partFadeClass(bucket);
        float fade = plight[idx * PART_LIGHT_VEC4 + 1u].w;
        if (ssr.vol_eye.w > 1.5) fade *= 1.0 - clamp(p.glow.z, 0.0, 1.0);   // (#721: into the volume)
        col = partApplyFade(col, fcls, fade);
        if (partInvisible(col, fcls)) { collapseParticle(bucket, p.pos_age.xyz); return; }
    }
    uint lidx = idx;   // whose lighting record this vertex takes
#endif
    // SL glow (start->end over life). Only the glow pass reads v_glow.
    v_glow = mix(p.glow.x, p.glow.y, t);

    // Camera world-space basis (rows of the view rotation).
    vec3 cam_fwd = -vec3(pc.u_inv_view_proj[0][2], pc.u_inv_view_proj[1][2], pc.u_inv_view_proj[2][2]);

    vec3 wpos;
    vec2 uv;

    bool  is_ribbon = ((flags & F_RIBBON) != 0u);

    if (is_ribbon)
    {
        // LL ribbon (LLVOPartGroup::getGeometry): a quad from this particle's CURRENT
        // position to its PARENT's (the particle its source emitted next), each end
        // spanning its own width axis (source +Z at spawn) x its own scale; the newest
        // particle connects to the live source. The chain therefore follows the
        // particles as they fly (a leash stretches collar -> holder, sagging under its
        // acceleration). A parent slot that died / was reused (serial mismatch) ends the
        // strip there, as LL unlinks a deleted particle.
        vec3  P0 = p.pos_age.xyz;
        vec3  A0 = p.axis.xyz;
        float s0 = scl.x;
        vec3  P1, A1;
        float s1;
        bool  have = false;
        const uint par  = floatBitsToUint(p.prev_pos.x);
        const uint pser = floatBitsToUint(p.prev_pos.y);
        if (par < maxp && parts[par].misc.w != 0u && parts[par].pad.x == pser)
        {
            Particle q = parts[par];
            float qmax = q.vel_maxage.w;
            float qt = (qmax > 0.0) ? clamp(q.pos_age.w / qmax, 0.0, 1.0) : 0.0;
            P1 = q.pos_age.xyz;
            A1 = q.axis.xyz;
            s1 = ((q.misc.x & F_INTERP_SCALE) != 0u) ? mix(q.scale.x, q.scale.z, qt) : q.scale.x;
            have = true;
#ifdef PART_VERTEX_LIGHT
            if (corner < 2) lidx = par;   // the parent end is lit where the parent is
#endif
        }
        else if (par == 0xFFFFFFFFu)
        {
            // Newest of its chain: to the live source, if the emitter slot is still its.
            const Emitter em = emit[p.misc.z];
            if (em.misc.w == p.pad.y)
            {
                P1 = em.pos.xyz;
                const vec4 q = em.src_rot;   // LL: source render rotation, no omega
                const vec3 up = vec3(0.0, 0.0, 1.0);
                A1 = up + 2.0 * cross(q.xyz, cross(q.xyz, up) + q.w * up);
                s1 = p.scale.x;   // LL: the particle's start scale
                have = true;
            }
        }
        const float seg = have ? length(P1 - P0) : 0.0;
        if (!have || seg > 64.0)
        {
            // No partner (source gone, parent died) or a teleport-length jump: nothing.
            collapseParticle(bucket, P0);
            return;
        }
        // Corners 0/1 = the parent end (+/- its axis), 2/3 = this end (LL p2 p3 p0 p1).
        const vec3  base = (corner < 2) ? P1 : P0;
        const vec3  ax   = (corner < 2) ? A1 : A0;
        const float hw   = 0.5 * ((corner < 2) ? s1 : s0);
        wpos = base + ax * (((corner & 1) == 0) ? hw : -hw);
        // Texture runs ALONG the trail: u follows the length, v the width.
        uv = vec2((corner < 2)        ? 0.0 : 1.0,
                  ((corner & 1) == 0) ? 0.0 : 1.0);
    }
    else
    {
        // Camera-facing billboard. Strip corners 0..3 -> (-,-),(+,-),(-,+),(+,+).
        vec2 c = vec2(((corner & 1) == 0) ? -0.5 : 0.5,
                      (corner < 2)        ? -0.5 : 0.5);
        vec3 right = vec3(pc.u_inv_view_proj[0][0], pc.u_inv_view_proj[1][0], pc.u_inv_view_proj[2][0]);
        vec3 up    = vec3(pc.u_inv_view_proj[0][1], pc.u_inv_view_proj[1][1], pc.u_inv_view_proj[2][1]);
        if ((flags & F_FOLLOW_VELOCITY) != 0u)
        {
            // Orient the quad's height along the particle's screen-space
            // motion so streak particles trail along their velocity.
            vec3 vel = p.vel_maxage.xyz;
            vec3 vscreen = vel - dot(vel, cam_fwd) * cam_fwd;
            if (dot(vscreen, vscreen) > 1.0e-8)
            {
                up    = normalize(vscreen);
                right = normalize(cross(up, cam_fwd));
            }
        }
        wpos = p.pos_age.xyz + right * (c.x * scl.x) + up * (c.y * scl.y);
        uv = c + 0.5;
    }

    gl_Position = pc.u_view_proj * vec4(wpos - pc.u_origin.xyz, 1.0);   // camera-relative (#555)
    v_uv = uv;
    // Fullbright color straight through; SL particle glow gets a proper
    // emissive/bloom path later rather than a brightness hack here (the
    // earlier multiply double-counted with additive blend buckets).
    v_tint = col;
    // bit 8: SL EMISSIVE (fullbright); otherwise the FS lights it (LL alpha pool).
    v_flags = bucket | (((flags & 0x100u) != 0u) ? 0x100u : 0u);
    v_world_normal = vec3(0.0, 0.0, 1.0);
    v_world_pos = wpos;
    // The particle's texture index: the sim refreshes it from the live emitter slot while
    // the slot is still this particle's source (already-alive particles update once the
    // texture finishes loading) and keeps it once the slot is recycled for another source
    // (#693: the live row then names that source's texture).
    v_tex = p.tex_handle.x;
#ifdef PART_VERTEX_LIGHT
    {
        const uint o = lidx * PART_LIGHT_VEC4;
        const vec4 r0 = plight[o], r1 = plight[o + 1u], r2 = plight[o + 2u];
        v_plight = r0.rgb;
        v_plocal = r1.rgb;
        v_phaze  = vec4(r2.rgb, r0.a);
    }
#endif
}
