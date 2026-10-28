// vkclean — shared UV generation for the world vertex shaders (port of cleanGPUV's
// texAnimUV / planarProjectionUV). Instance flag bits: 64 = planar texgen,
// 1024 = GPU texture animation (uv_matrix holds the llSetTextureAnim param block,
// lane map in LLVolumeGeometryManager::registerCleanFace).
#ifndef CLEAN_UV_COMMON_GLSL
#define CLEAN_UV_COMMON_GLSL

#define TA_ON        0x01u
#define TA_LOOP      0x02u
#define TA_REVERSE   0x04u
#define TA_PING_PONG 0x08u
#define TA_SMOOTH    0x10u
#define TA_ROTATE    0x20u
#define TA_SCALE     0x40u

vec2 texAnimUV(vec2 uv, mat4 pm, float u_anim_time)
{
    uint  mode   = uint(pm[0][0] + 0.5);
    float sizeX  = pm[0][1];
    float sizeY  = pm[0][2];
    float startf = pm[0][3];
    float lenf   = pm[1][0];
    float rate   = pm[1][1];
    float te_os  = pm[1][2];
    float te_ot  = pm[1][3];
    float te_ss  = pm[2][0];
    float te_st  = pm[2][1];
    float te_rot = pm[2][2];
    float t0     = pm[2][3];

    // ---- frame counter (LLViewerTextureAnim::animateTextures) ----
    float num_frames = (lenf > 0.0) ? lenf : max(1.0, sizeX * sizeY);
    float full_length;
    if ((mode & TA_PING_PONG) != 0u)
    {
        if      ((mode & TA_SMOOTH) != 0u) full_length = 2.0 * num_frames;
        else if ((mode & TA_LOOP)   != 0u) full_length = max(1.0, 2.0 * num_frames - 2.0);
        else                               full_length = max(1.0, 2.0 * num_frames - 1.0);
    }
    else full_length = num_frames;

    float fc = (u_anim_time - t0) * rate;   // stateless == LL's timer accumulation
    if ((mode & TA_LOOP) != 0u) fc = mod(fc, full_length);
    else                        fc = min(full_length - 1.0, fc);
    if ((mode & TA_SMOOTH) == 0u)
        fc = min(full_length - 1.0, floor(fc + 0.01));
    if ((mode & TA_PING_PONG) != 0u && fc >= num_frames)
    {
        if ((mode & TA_SMOOTH) != 0u) fc = num_frames - (fc - num_frames);
        else                          fc = (num_frames - 1.99) - (fc - num_frames);
    }
    if ((mode & TA_REVERSE) != 0u)
    {
        if ((mode & TA_SMOOTH) != 0u) fc = num_frames - fc;
        else                          fc = (num_frames - 0.99) - fc;
    }
    fc += startf;
    if ((mode & TA_SMOOTH) == 0u) fc = floor(fc + 0.5);   // ll_round

    // ---- channel selection (anim-driven vs static TE) ----
    float rot = te_rot, ss = te_ss, st = te_st, os = te_os, ot = te_ot;
    if ((mode & TA_ROTATE) != 0u)
    {
        rot = fc;
    }
    else if ((mode & TA_SCALE) != 0u)
    {
        ss = fc; st = fc;
    }
    else
    {
        // sprite-sheet translate (+implicit scale when a grid is given)
        if (sizeX > 0.0 && sizeY > 0.0)
        {
            ss = 1.0 / sizeX;  st = 1.0 / sizeY;
            // <GMDX> Row and column exactly, as LL does on the CPU ((S32)(fc / sizeX),
            // fmodf). GPU division is approximate: 6.0 / 6.0 could land just under 1.0,
            // giving row 0 and column 6 -- off the end of the row, wrapped to column 0 of
            // the row above. A flipbook flashed the wrong frame at every row change.
            float x_frame, y_frame;
            if ((mode & TA_SMOOTH) == 0u)
            {
                const int fi = int(fc);              // whole after the ll_round above
                const int sx = max(int(sizeX + 0.5), 1);
                y_frame = float(fi / sx);
                x_frame = float(fi - (fi / sx) * sx);
            }
            else
            {
                // Smooth: fc is continuous; nudge the row division so a value a hair under
                // a row boundary can't take the previous row with a full-width column.
                y_frame = floor((fc + 1.0e-4) / sizeX);
                x_frame = max(fc - y_frame * sizeX, 0.0);
            }
            // </GMDX>
            os = (-0.5 + 0.5 * ss) + x_frame * ss;
            ot = ( 0.5 - 0.5 * st) - y_frame * st;
        }
        else
        {
            // No frame grid: LL's animateTextures reports only TRANSLATE here, so
            // LLVOVolume::animateTextures keeps the face's own TE scale (its
            // scale_s/t = 1 is local to the offset maths). Replacing the TE
            // repeats with 1x1 stretched e.g. a 1.6 x 200 rain texture into
            // smears. Offset: frame counter in s, 0 in t (TE offset ignored).
            os = (-0.5 + 0.5) + fc;
            ot = 0.0;
        }
    }

    // ---- LL tex_mat composition (row-vector: T(-.5) * R(-z,rot) * S * T(off+.5)) ----
    float c = cos(rot), sn = sin(rot);
    vec2 pcen = uv - vec2(0.5);
    vec2 prot = vec2(pcen.x * c + pcen.y * sn,
                    -pcen.x * sn + pcen.y * c);
    precise vec2 r = prot * vec2(ss, st) + vec2(os + 0.5, ot + 0.5);   // (precise: see cleanFaceUV)
    return r;
}

vec2 planarProjectionUV(vec3 n, vec3 vec)
{
    float d = n.x;
    vec3 binormal;
    if (d >= 0.5 || d <= -0.5)
    {
        binormal = vec3(0.0, (d < 0.0) ? -1.0 : 1.0, 0.0);
    }
    else
    {
        binormal = (n.y > 0.0) ? vec3(-1.0, 0.0, 0.0) : vec3(1.0, 0.0, 0.0);
    }
    vec3 tangent = cross(binormal, n);
    precise vec2 tc;   // (precise: see cleanFaceUV)
    tc.y =-((dot(tangent,  vec) * 2.0) - 0.5);
    tc.x = 1.0 + ((dot(binormal, vec) * 2.0) - 0.5);
    return tc;
}

#include "clean_mat_uv.glsl"

// RAW texcoord: texgen only (planar or asset uv), before the anim / TE / glTF transform.
// Per-map transforms (clean_mat_uv.glsl) apply to this.
vec2 cleanFaceRawUV(uint flags, vec2 texcoord, vec3 local_normal, vec3 local_pos, mat4 model)
{
    if ((flags & 64u) != 0u)
    {
        vec3 obj_scale = vec3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
        return planarProjectionUV(normalize(local_normal), local_pos * obj_scale);
    }
    return texcoord;
}

// Final UV: texgen (planar or asset uv) first, then the anim or the TE transform.
// PRECISE (#1088): the result is v_uv, which the mask pre-pass's alpha test and the EQUAL
// lit / G-buffer FS's must sample identically -- from different VS modules. `invariant v_uv`
// alone did not hold on an AMD Windows driver (holey alpha-masked text), so the UV chain
// carries NoContraction too (here and in texAnimUV / planarProjectionUV: a call's body is
// outside the reach of a caller's precise variable).
vec2 cleanFaceUV(uint flags, mat4 uv_matrix, vec2 texcoord, vec3 local_normal, vec3 local_pos,
                 mat4 model, float time)
{
    vec2 base_uv = texcoord;
    if ((flags & 64u) != 0u)
    {
        vec3 obj_scale = vec3(length(model[0].xyz), length(model[1].xyz), length(model[2].xyz));
        base_uv = planarProjectionUV(normalize(local_normal), local_pos * obj_scale);
    }
    if ((flags & 1024u) != 0u)
        return texAnimUV(base_uv, uv_matrix, time);
    precise vec2 r = (uv_matrix * vec4(base_uv, 0.0, 1.0)).xy;
    return r;
}

// Tangent for normal mapping in the TEXTURE's frame (LL tangent_space_transform):
// the mesh tangent/bitangent are d(pos)/du, d(pos)/dv of the RAW uvs; the face's
// uv_matrix (TE repeats/rotation/flip or the glTF transform) remaps uv' = A*uv, so
// d(pos)/du' = T*Ai[0][0] + B*Ai[0][1] with Ai = inverse(A); handedness is recomputed.
// Planar texgen (64) and GPU texture animation (1024, uv_matrix holds anim params)
// return a ZERO tangent: the FS then derives the frame from screen-space derivatives.
vec4 cleanFaceTangent(uint flags, mat4 uvm, vec3 n, vec4 t)
{
    if ((flags & (64u | 1024u)) != 0u || dot(t.xyz, t.xyz) < 1e-12) return vec4(0.0);
    vec3 b = cross(n, t.xyz) * t.w;
    mat2 A = mat2(uvm[0].xy, uvm[1].xy);
    if (abs(determinant(A)) < 1e-12) return t;
    mat2 Ai = inverse(A);
    vec3 T2 = t.xyz * Ai[0][0] + b * Ai[0][1];
    vec3 B2 = t.xyz * Ai[1][0] + b * Ai[1][1];
    float w = (dot(cross(n, T2), B2) < 0.0) ? -1.0 : 1.0;
    float l = length(T2);
    return (l > 1e-8) ? vec4(T2 / l, w) : vec4(0.0);
}

#endif
