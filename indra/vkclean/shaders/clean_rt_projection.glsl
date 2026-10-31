#ifndef CLEAN_RT_PROJECTION_GLSL
#define CLEAN_RT_PROJECTION_GLSL

bool rtViewOrtho()
{
    const mat4 M = ssr.views[CLEAN_VIEW].taa_cur_vp;
    return M[0][3] == 0.0 && M[1][3] == 0.0 && M[2][3] == 0.0;
}

float rtOrthoRange()
{
    const mat4 M = ssr.views[CLEAN_VIEW].taa_cur_vp;
    return 1.0 / max(length(vec3(M[0][2], M[1][2], M[2][2])), 1e-12);
}

float rtDepthErrDist(float camd)
{
    return rtViewOrtho() ? rtOrthoRange() : camd;
}

float rtPreviousViewDepth(vec4 clip)
{
    return rtViewOrtho() ? (ssr.views[CLEAN_VIEW].taa_cur_vp[3][2] - clip.z) * rtOrthoRange() : clip.w;
}

#endif
