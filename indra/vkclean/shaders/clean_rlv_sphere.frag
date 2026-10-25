#version 450
// vkclean — RLVa @setsphere vision restriction (port of RLVa rlvF.glsl, Kitty Barnett,
// LGPL 2.1). Display space, after AA and before the Lightbox final pass; one draw per
// effect, two (horizontal then vertical) for the blur modes. The distance is measured
// in view space from the sphere origin to the scene point reconstructed from the
// render-size world depth (unjittered as clean_depth_up.comp).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D diffuseRect;   // display-space frame (linear clamp)
layout(set = 0, binding = 1) uniform sampler2D uDepth;        // world depth, render size (texelFetch)

layout(push_constant) uniform Push {
    vec4 unproj;   // inv_proj[0], inv_proj[5], TAA jitter (render px, y down)
    vec4 lin;      // inv_proj[10], [14], [11], [15]: view z / w from depth
    vec4 origin;   // sphere origin (view space) .xyz, mode
    vec4 sphere;   // value min, dist min, value max, dist max
    vec4 params;   // effect params (= colour for blend)
    vec4 misc;     // dist extend min, max, blur direction .xy
} pc;

#define SPHERE_ORIGIN     pc.origin.xyz
#define SPHERE_DISTMIN    pc.sphere.y
#define SPHERE_DISTMAX    pc.sphere.w
#define SPHERE_VALUEMIN   pc.sphere.x
#define SPHERE_VALUEMAX   pc.sphere.z
#define SPHERE_PARAMS     pc.params
#define BLUR_DIRECTION    pc.misc.zw

vec2 screen_res;

vec3 blur13(vec2 tc, vec2 direction)
{
    vec4 color = vec4(0.0);
    vec2 off1 = vec2(1.411764705882353) * direction;
    vec2 off2 = vec2(3.2941176470588234) * direction;
    vec2 off3 = vec2(5.176470588235294) * direction;

    color += texture(diffuseRect, tc) * 0.1964825501511404;

    color += texture(diffuseRect, tc + off1 / screen_res) * 0.2969069646728344;
    color += texture(diffuseRect, tc - off1 / screen_res) * 0.2969069646728344;

    color += texture(diffuseRect, tc + off2 / screen_res) * 0.09447039785044732;
    color += texture(diffuseRect, tc - off2 / screen_res) * 0.09447039785044732;

    color += texture(diffuseRect, tc + off3 / screen_res) * 0.010381362401148057;
    color += texture(diffuseRect, tc - off3 / screen_res) * 0.010381362401148057;

    return color.xyz;
}

const float pi = 3.14159265;

// http://callumhay.blogspot.com/2010/09/gaussian-blur-shader-glsl.html
// Kernel size is the script's param.x; taps per side are capped (unbounded loop).
vec3 blurVariable(vec2 tc, float kernelSize, vec2 direction, float strength)
{
    float numBlurPixelsPerSide = min(floor(kernelSize / 2.0), 32.0);

    // Incremental Gaussian Coefficent Calculation (See GPU Gems 3 pp. 877 - 889)
    vec3 incrementalGaussian;
    incrementalGaussian.x = 1.0 / (sqrt(2.0 * pi) * strength);
    incrementalGaussian.y = exp(-0.5 / (strength * strength));
    incrementalGaussian.z = incrementalGaussian.y * incrementalGaussian.y;

    vec4 avgValue = vec4(0.0);
    float coefficientSum = 0.0;

    avgValue += texture(diffuseRect, tc) * incrementalGaussian.x;
    coefficientSum += incrementalGaussian.x;
    incrementalGaussian.xy *= incrementalGaussian.yz;

    for (float i = 1.0; i <= numBlurPixelsPerSide; i++)
    {
        avgValue += texture(diffuseRect, tc - i * direction / screen_res) * incrementalGaussian.x;
        avgValue += texture(diffuseRect, tc + i * direction / screen_res) * incrementalGaussian.x;
        coefficientSum += 2.0 * incrementalGaussian.x;
        incrementalGaussian.xy *= incrementalGaussian.yz;
    }

    return (avgValue / coefficientSum).rgb;
}

vec3 chromaticAberration(vec2 tc, vec2 redDrift, vec2 blueDrift, float strength)
{
    vec3 sourceColor = texture(diffuseRect, tc).rgb;

    vec3 driftColor;
    driftColor.r = texture(diffuseRect, tc + redDrift / screen_res).r;
    driftColor.g = sourceColor.g;
    driftColor.b = texture(diffuseRect, tc + blueDrift / screen_res).b;

    return mix(sourceColor, driftColor, strength);
}

void main()
{
    screen_res = vec2(textureSize(diffuseRect, 0));
    vec2 fragTC = v_uv;

    ivec2 rsize = textureSize(uDepth, 0);
    ivec2 q = clamp(ivec2(floor(fragTC * vec2(rsize) + pc.unproj.zw)), ivec2(0), rsize - 1);
    float z = texelFetch(uDepth, q, 0).r;
    vec2 ndc = fragTC * 2.0 - 1.0;
    float w = pc.lin.z * z + pc.lin.w;
    vec3 fragPosView = vec3(ndc.x * pc.unproj.x, ndc.y * pc.unproj.y, pc.lin.x * z + pc.lin.y) / w;
    float distance = length(fragPosView - SPHERE_ORIGIN);

    float effectStrength = SPHERE_VALUEMIN + (SPHERE_VALUEMAX - SPHERE_VALUEMIN)
                         * ((distance - SPHERE_DISTMIN) / max(SPHERE_DISTMAX - SPHERE_DISTMIN, 1e-6));
    if (distance < SPHERE_DISTMIN)
    {
        effectStrength = (pc.misc.x > 0.5) ? SPHERE_VALUEMIN : 0.0;
    }
    else if (distance > SPHERE_DISTMAX)
    {
        effectStrength = (pc.misc.y > 0.5) ? SPHERE_VALUEMAX : 0.0;
    }

    int mode = int(pc.origin.w + 0.5);
    vec3 fragColor;
    if (mode == 0)          // Blend
    {
        fragColor = texture(diffuseRect, fragTC).rgb;
        fragColor = mix(fragColor, SPHERE_PARAMS.rgb, effectStrength);
    }
    else if (mode == 1)     // Blur (fixed)
    {
        fragColor = blur13(fragTC, BLUR_DIRECTION * effectStrength);
    }
    else if (mode == 2)     // Blur (variable)
    {
        fragColor = texture(diffuseRect, fragTC).rgb;
        if (effectStrength > 0.0)
            fragColor = blurVariable(fragTC, SPHERE_PARAMS.x, BLUR_DIRECTION, effectStrength);
    }
    else if (mode == 3)     // Chromatic aberration
    {
        fragColor = chromaticAberration(fragTC, SPHERE_PARAMS.xy, SPHERE_PARAMS.zw, effectStrength);
    }
    else if (mode == 4)     // Pixelate
    {
        effectStrength = sign(effectStrength);
        float pixelWidth = max(1.0, round(SPHERE_PARAMS.x * effectStrength)) / screen_res.x;
        float pixelHeight = max(1.0, round(SPHERE_PARAMS.y * effectStrength)) / screen_res.y;
        fragTC = vec2(pixelWidth * floor(fragTC.x / pixelWidth), pixelHeight * floor(fragTC.y / pixelHeight));
        fragColor = texture(diffuseRect, fragTC).rgb;
    }
    else
    {
        fragColor = texture(diffuseRect, fragTC).rgb;
    }

    outColor = vec4(fragColor, 1.0);
}
