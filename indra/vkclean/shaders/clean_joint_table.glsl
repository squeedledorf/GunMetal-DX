// vkclean -- the avatar joint WORLD table (LLCleanSkinStore::AvatarJoints, one row of 216
// joints per avatar slot; the CPU stream, the anim compute's output and the previous-frame
// copy all use it). Each joint is an AFFINE matrix: the 4x4's last row is always
// (0, 0, 0, 1), so only 12 floats (48 B, was 64 B) are stored, as a std430 mat3x4 whose
// column r is ROW r of the 4x4's upper 3x4:
//     W[r] = vec4(M[0][r], M[1][r], M[2][r], M[3][r])        (M column-major, as before)
// jointWorld() rebuilds the exact mat4 (no precision change: the dropped row is constant).
struct AvatarJoints { mat3x4 world[216]; };   // LLCleanSkinStore::MAX_AVATAR_JOINTS

mat4 jointWorld(mat3x4 W)
{
    return mat4(vec4(W[0].x, W[1].x, W[2].x, 0.0),
                vec4(W[0].y, W[1].y, W[2].y, 0.0),
                vec4(W[0].z, W[1].z, W[2].z, 0.0),
                vec4(W[0].w, W[1].w, W[2].w, 1.0));
}

mat3x4 jointPack(mat4 M)
{
    return mat3x4(vec4(M[0][0], M[1][0], M[2][0], M[3][0]),
                  vec4(M[0][1], M[1][1], M[2][1], M[3][1]),
                  vec4(M[0][2], M[1][2], M[2][2], M[3][2]));
}
