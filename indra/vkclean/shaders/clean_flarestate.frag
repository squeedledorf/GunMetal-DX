#version 450
// vkclean — lens flare sun state (Lightbox; port of upstream Alchemy
// class1/alchemy/lensFlareStateF.glsl, verbatim below). A 2x1 target: texel 0 =
// filtered sun drive colour + instability, texel 1 = bookkeeping. Ping-pongs with
// last frame's copy. Samples the linear HDR scene and the world depth around the sun.
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D diffuseRect;        // HDR scene (point)
layout(set = 0, binding = 1) uniform sampler2D depthMap;           // world depth
layout(set = 0, binding = 2) uniform sampler2D uLensFlareStateMap; // last frame's state
layout(push_constant) uniform StatePush {
    vec4 p0;   // sun uv.xy, visibility, occlusion radius
    vec4 p1;   // resolution.xy, dt, fade time
} pc;
#define uLensFlareSunPos          pc.p0.xy
#define uLensFlareSunVisibility   pc.p0.z
#define uLensFlareOcclusionRadius pc.p0.w
#define uResolution               pc.p1.xy
#define dt                        pc.p1.z
#define uLensFlareFadeTime        pc.p1.w




const vec3  LUMA         = vec3(0.2126, 0.7152, 0.0722);
const float GOLDEN_ANGLE = 2.39996322972865332;

// ---- Spatial kernel ---------------------------------------------------------
// A fixed Fermat spiral of 256 taps, weighted exp(-K_TAP r^2): tight enough that
// the unoccluded estimate is 98% of the disc's centre texel, dense enough that
// a straight edge through the centre reads within 0.04 of half at every angle
// (std 0.015) and no single tap carries more than 3.1% of the weight.
//
// The pattern is deliberately NOT rotated per frame. Against a fine occluder
// such as alpha-masked foliage every tap is a coin toss, and a pattern that
// moves re-tosses them all every frame: the flare then flickered with the
// camera and the trees perfectly still. A fixed pattern is exact for a still
// scene and changes continuously as the camera moves. 48 taps could only
// afford that with rotation (worst edge bias 0.14, 15% per tap); 256 cannot
// be told apart from the rotated pattern and cost two fragments a frame.
//
// Each tap is gated on its own: an occluded tap contributes nothing, an
// unoccluded one its overbright colour (only an HDR-bright sun flares, never
// a bright wall). Gating the mean colour instead switched the whole flare on
// and off at the threshold whenever the disc was mostly hidden.
const int   TAP_COUNT = 256;
const float K_TAP     = 8.0;
const float GATE      = 2.0;    // luminance below which a texel is not sun

// ---- FadeTime to the filter's rates -----------------------------------------
// A first-order fade of tau = FadeTime/3 under a slew of 1/FadeTime rises
// 10-90% in FadeTime; the fall is 0.6x that. The slew is the guarantee: a full
// off-on-off cycle cannot complete in less than 1.6 FadeTime whatever passes
// in front of the sun.
const float TAU_IN_MUL   = 1.0 / 3.0;   // x FadeTime, seconds
const float TAU_OUT_MUL  = 0.2;         // x FadeTime, seconds
const float SLEW_IN_MUL  = 1.0;         // / FadeTime, per second, relative to the reference
const float SLEW_OUT_MUL = 1.0 / 0.6;   // / FadeTime, per second

// ---- Temporal filter --------------------------------------------------------
const float DT_MAX      = 0.25; // a stall converges, it does not teleport
const float TAU_REF     = 2.0;  // reference luminance decay, seconds
const float STEP_THRESH = 0.1;  // displacement from the anchor that registers a step,
                                // relative to the unoccluded sun's brightness
const float GAIN        = 0.35; // instability added per direction reversal
const float TAU_I       = 1.0;  // instability decay, seconds
const float I_DEADZONE  = 0.30; // one reversal (an ordinary reveal) slows nothing
const float K_DAMP      = 20.0; // tau multiplier slope above the dead zone
const float SNAP_FLOOR  = 1e-4; // drive luminance below which a black target snaps to exact zero

float skyOf(float d)
{
    // Only the far layers count as unoccluded: the sun disc is pinned at
    // 0.999999 and the sky dome at 0.99999 (forward); mirrored under reverse-Z.
    // Main view is REVERSED-Z (#188): far = 0.
    return smoothstep(0.9999, 1.0, 1.0 - d);
}

void main()
{
    vec2  sun_uv    = uLensFlareSunPos;
    vec2  radius_uv = uLensFlareOcclusionRadius * vec2(uResolution.y / max(uResolution.x, 1.0), 1.0);

    float w_sum = 0.0;
    vec3  energy = vec3(0.0);
    float peak   = 0.0;
    for (int i = 0; i < TAP_COUNT; i++)
    {
        float r  = sqrt((float(i) + 0.5) / float(TAP_COUNT));
        float a  = float(i) * GOLDEN_ANGLE;
        vec2  t  = vec2(cos(a), sin(a)) * r;
        float w  = exp(-K_TAP * dot(t, t));
        w_sum += w;
        // A tap past the frame reads the nearest edge texel for depth and
        // colour: an occluder reaching the edge still occludes, and a sun just
        // out of view keeps its glow, so the streak survives across the edge
        // margin.
        vec2  uv  = clamp(sun_uv + t * radius_uv, vec2(0.0), vec2(1.0));
        float sky = skyOf(texture(depthMap, uv).r);
        if (sky > 0.0)
        {
            // Colour fetched only for unoccluded taps, so an occluder texel
            // never enters the estimate, not even times zero. This fetch sits
            // in non-uniform control flow, where implicit derivatives are
            // undefined; an explicit level keeps it defined on every driver.
            vec3  c    = textureLod(diffuseRect, uv, 0.0).rgb;
            float lum  = dot(c, LUMA);
            vec3  over = c * (max(lum - GATE, 0.0) / max(lum, 1e-4));
            energy += w * sky * over;
            peak    = max(peak, dot(over, LUMA));
        }
    }
    vec3 sun_drive = energy / w_sum;                // the visible sun's overbright colour
    vec3 target    = sun_drive * uLensFlareSunVisibility;

    // ---- Temporal filter, in luminance, RGB following ------------------------
    vec4  prev0  = texelFetch(uLensFlareStateMap, ivec2(0, 0), 0);
    vec4  prev1  = texelFetch(uLensFlareStateMap, ivec2(1, 0), 0);
    float dtc    = clamp(dt, 0.0, DT_MAX);
    float fade   = max(uLensFlareFadeTime, 0.05);
    float Lp     = dot(prev0.rgb, LUMA);
    float Lt     = dot(target, LUMA);
    float L_full = peak;                            // the sun as if fully unoccluded
    float L_ref  = max(max(prev1.g * exp(-dtc / TAU_REF), Lt), Lp);

    // Instability: direction reversals of the raw target within about a second.
    // A step is a move of more than STEP_THRESH away from the anchor (the value
    // at the last step), measured against the unoccluded sun's brightness
    // rather than the current level, so neither a crossing spread over many
    // frames nor the tap jitter behind a thin post can hide or fake one. A
    // fence post train reverses every crossing; a single reveal does not.
    float inst_packed = prev0.a;
    float I      = abs(inst_packed);
    float s_prev = sign(inst_packed);
    float anchor = prev1.b;
    float delta  = (Lt - anchor) / max(max(L_full, L_ref), 1e-6);
    float s_now  = (abs(delta) > STEP_THRESH) ? sign(delta) : 0.0;
    if (s_now != 0.0)
    {
        anchor = Lt;
    }
    bool  reversal = (s_now != 0.0) && (s_prev != 0.0) && (s_now != s_prev);
    I = min(I * exp(-dtc / TAU_I) + (reversal ? GAIN : 0.0), 1.0);
    float s_store = (s_now != 0.0) ? s_now : s_prev;
    inst_packed = (s_store != 0.0) ? s_store * max(I, 1e-3) : 0.0;

    // Fade: asymmetric time constants from FadeTime, slowed while unstable,
    // and a slew cap relative to the reference so no frame moves the drive by
    // more than a bounded fraction of the sun's recent brightness.
    bool  rising = Lt > Lp;
    float tau    = fade * (rising ? TAU_IN_MUL : TAU_OUT_MUL);
    tau *= 1.0 + K_DAMP * max(I - I_DEADZONE, 0.0);
    float alpha  = 1.0 - exp(-dtc / tau);
    float slew   = (rising ? SLEW_IN_MUL : SLEW_OUT_MUL) / fade;
    float cap    = slew * dtc * L_ref;
    float diff   = abs(Lt - Lp);
    if (diff > 1e-9)
    {
        alpha = min(alpha, cap / diff);
    }
    vec3 drive = max(mix(prev0.rgb, target, alpha), vec3(0.0));
    // A geometric decay never reaches zero in a half-float target (it pins at
    // a denormal) and the reader's early-out needs exact black: snap once the
    // target is black and the drive is far below anything visible.
    if (Lt <= 0.0 && dot(drive, LUMA) < SNAP_FLOOR)
    {
        drive = vec3(0.0);
    }

    if (gl_FragCoord.x < 1.0)
    {
        outColor = vec4(drive, inst_packed);
    }
    else
    {
        outColor = vec4(Lt, L_ref, anchor, dtc);
    }
}
