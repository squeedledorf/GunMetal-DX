// One linear camera-segment transport. Requires physical sky math and frame info.
#ifndef CLEAN_MEDIUM_GLSL
#define CLEAN_MEDIUM_GLSL
layout(set = 0, binding = 59) uniform sampler3D uMediumL[CLEAN_MAX_VIEWS];
layout(set = 0, binding = 60) uniform sampler3D uMediumT[CLEAN_MAX_VIEWS];
layout(set = 0, binding = 61) uniform sampler2D uMediumAirL[CLEAN_MAX_VIEWS];
layout(set = 0, binding = 62) uniform sampler2D uMediumAirT[CLEAN_MAX_VIEWS];
layout(set = 0, binding = 63) uniform sampler2D uPkMs;
#ifdef CLEAN_MEDIUM_PASS
layout(constant_id = 21) const uint kMediumEye = 0u;
#define MEDIUM_EYE kMediumEye
#else
#define MEDIUM_EYE CLEAN_VIEW
#endif
void weatherPrecipMedium(vec3 P,vec2 wind,float fall,float time,float motion,inout vec3 scattering,inout float extinction);
struct CameraTransport { vec3 L, T; float heightTau; };
CameraTransport cmIdentity() { return CameraTransport(vec3(0.0), vec3(1.0),0.0); }
bool cmOn() { return physkyOn() && ssr.haze_mode.x > 0.5; }
float cmPhase(float c, float g)
{
    return (1.0 - g*g) / (4.0 * PK_PI * pow(max(1.0 + g*g - 2.0*g*c, 1e-4), 1.5));
}
float cmExpIntegral(float x)
{
    return abs(x) < 1e-3 ? 1.0 - x*0.5 + x*x/6.0 - x*x*x/24.0 : (1.0 - exp(-x))/x;
}
float cmMistHash(ivec3 c)
{
    uint h = uint(c.x) * 1664525u + uint(c.y) * 22695477u + uint(c.z) * 1103515245u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return float(h & 0x00ffffffu) / 16777215.0;
}
float cmMistNoise(vec3 p, int period)
{
    const vec3 cell = floor(p);
    const vec3 f = p - cell;
    const vec3 u = f * f * (3.0 - 2.0 * f);
    const ivec3 a = ivec3(mod(cell,vec3(float(period))));   // (GLSL % is undefined below 0)
    const ivec3 b = (a + ivec3(1)) % period;
    return mix(mix(mix(cmMistHash(ivec3(a.x, a.y, a.z)), cmMistHash(ivec3(b.x, a.y, a.z)), u.x),
                   mix(cmMistHash(ivec3(a.x, b.y, a.z)), cmMistHash(ivec3(b.x, b.y, a.z)), u.x), u.y),
               mix(mix(cmMistHash(ivec3(a.x, a.y, b.z)), cmMistHash(ivec3(b.x, a.y, b.z)), u.x),
                   mix(cmMistHash(ivec3(a.x, b.y, b.z)), cmMistHash(ivec3(b.x, b.y, b.z)), u.x), u.y), u.z);
}
void cmHeightProfile(vec3 P,inout float base,inout float multiplier)
{
    if(ssr.haze_mist[1].z<=0.0) return;
    const float size=max(ssr.haze_mist[1].x,1.0);
    const vec3 q=P/size;
    const float banks=cmMistNoise((q+vec3(ssr.haze_mist[0].xy,0.0)/size)*vec3(1.0,1.0,2.0),64);
    const float wisps=cmMistNoise((q+vec3(ssr.haze_mist[0].zw,0.0)/size)*3.0,192);
    base+=(banks-0.5)*ssr.haze_mist[1].y;
    multiplier=mix(1.0,smoothstep(0.25,0.75,banks*0.7+wisps*0.3)*1.5,clamp(ssr.haze_mist[1].z,0.0,1.0));
}
// Integrate from the LOWER altitude end. Split water/base crossings explicitly.
float cmHeightTau(vec3 O, vec3 V, float begin, float end)
{
    begin = max(begin, ssr.haze.z);
    if (end <= begin || ssr.haze.x <= 0.0) return 0.0;
    const float water=ssr.haze_mode.z,H=max(ssr.haze.y,0.5);
    float base=ssr.medium[MEDIUM_EYE].fog.z,multiplier=1.0;
    cmHeightProfile(O+V*((begin+end)*0.5),base,multiplier);
    if (V.z == 0.0)
        return O.z < water ? 0.0 : multiplier*ssr.haze.x * (end-begin) * exp(-max(O.z-base,0.0)/H);
    const float crossing = (water-O.z)/V.z;
    if (V.z > 0.0) begin = max(begin,crossing); else end = min(end,crossing);
    if (end <= begin) return 0.0;
    const float za=O.z+V.z*begin, zb=O.z+V.z*end;
    const float low=min(za,zb), distance=end-begin;
    const float dz=abs(V.z)*distance;
    if(dz==0.0) return multiplier*ssr.haze.x*distance*exp(-max(low-base,0.0)/H);
    const float below=clamp(base-low,0.0,dz), above=dz-below;
    return multiplier*ssr.haze.x*distance*(below/dz+exp(-max(low-base,0.0)/H)*(above/dz)*cmExpIntegral(above/H));
}
vec3 cmPlanet(vec3 P)
{
    return vec3(0.0,0.0,pkBottom()+max(P.z,0.0)*0.001);
}
vec3 cmMs(vec3 pos, vec3 lightDir)
{
    const float h = length(pos);
    vec2 uv = clamp(vec2(dot(lightDir,pos/h)*0.5+0.5,(h-pkBottom())/(pkTop()-pkBottom())),0.0,1.0);
    uv = vec2(pkUnitToSub(uv.x,PK_MS_RES),pkUnitToSub(uv.y,PK_MS_RES));
    return textureLod(uPkMs,uv,0.0).rgb;
}
// Physical coefficients are /km; convert ONCE to /m. Source light is already linear.
// A light needs one transmittance and one MS tap; at most four taps for sun+moon.
void cmLightPlanet(vec3 pos, vec3 V, vec3 directVisibility, float ambientVisibility,
             out vec3 airExt, out vec3 airSource, out vec3 heightSource)
{
    const PkMedium m = pkMedium(pos);
    const vec3 up=pos/length(pos);
    airExt = m.extinction*0.001;
    airSource = m.scattering*0.001*pkAmbient()*ambientVisibility;
    heightSource = 0.9*pkAmbient()*ambientVisibility;
    for (int i=0;i<2;++i)
    {
        if (i==1 && !physkyMoonOn()) break;
        const vec3 ld = i==0 ? pkLightDir() : physkyMoonDir();
        const vec3 irradiance = pkLight()*(i==0 ? 1.0 : nightMoonSky());
        const float lit = pkRaySphere(pos+up*PK_PLANET_OFFSET,ld,pkBottom())>=0.0 ? 0.0 : 1.0;
        const vec3 beam = textureLod(uPkTrans,pkTransUv(length(pos),dot(ld,up)),0.0).rgb * lit * directVisibility;
        const vec3 ms = cmMs(pos,ld);
        const float mu=dot(V,ld);
        airSource += irradiance*(beam*(m.scatRay*pkRayleighPhase(mu)+m.scatMie*pkMiePhase(pkMieG(),mu))+ms*m.scattering)*0.001;
        heightSource += 0.9*irradiance*(beam*cmPhase(mu,0.6)+ms);
    }
}
void cmLight(vec3 P,vec3 V,vec3 directVisibility,float ambientVisibility,
             out vec3 airExt,out vec3 airSource,out vec3 heightSource)
{
    cmLightPlanet(cmPlanet(P),V,directVisibility,ambientVisibility,airExt,airSource,heightSource);
}
void cmSlab(inout CameraTransport t, vec3 extinction, vec3 source, float distance)
{
    const vec3 optical=max(extinction,vec3(0.0))*distance;
    const vec3 tr=exp(-optical);
    const vec3 integral=vec3(cmExpIntegral(optical.x),cmExpIntegral(optical.y),cmExpIntegral(optical.z))*distance;
    t.L += t.T*source*integral;
    t.T *= tr;
}
// Both water and height-start boundaries are exact; homogeneous scaling applies
// the SAME remaining height optical-depth budget to extinction and source.
void cmStep(inout CameraTransport combined, inout CameraTransport air,
            vec3 O, vec3 V, float begin, float end, inout float usedHeight,
            vec3 localSource, float localExt, vec3 heightSource, vec3 airSource, vec3 airExt)
{
    if (V.z!=0.0)
    {
        const float crossing=(ssr.haze_mode.z-O.z)/V.z;
        if (V.z>0.0) begin=max(begin,crossing); else end=min(end,crossing);
    }
    else if (O.z<ssr.haze_mode.z) return;
    const float ds=max(end-begin,0.0);
    if (ds<=0.0) return;
    const float raw=cmHeightTau(O,V,begin,end);
    const float height=min(raw,max(ssr.haze.w-usedHeight,0.0));
    usedHeight+=height; combined.heightTau=usedHeight;
    const float sigma=height/ds;
    cmSlab(combined,airExt+vec3(sigma+max(localExt,0.0)),airSource+sigma*heightSource+localSource,ds);
    cmSlab(air,airExt,airSource,ds);
}
CameraTransport cmFiniteWithAir(vec3 O,vec3 V,float begin,float end,float usedHeight,out CameraTransport air)
{
    CameraTransport t=cmIdentity();t.heightTau=usedHeight;air=cmIdentity();
    // Fixed eight slabs, split at artistic start and water to avoid sampling a source
    // below water for a visible air segment. Heights use their analytic integral.
    for(int i=0;i<8;++i)
    {
        float a=mix(begin,end,float(i)/8.0), b=mix(begin,end,float(i+1)/8.0);
        if(V.z!=0.0)
        {
            const float crossWater=(ssr.haze_mode.z-O.z)/V.z;
            if(V.z>0.0) a=max(a,crossWater); else b=min(b,crossWater);
        }
        if(b<=a) continue;
        vec3 ext,src,height;
        cmLight(O+V*((a+b)*0.5),V,vec3(1.0),1.0,ext,src,height);
        vec3 localScattering=vec3(0.0); float localExtinction=0.0;
        weatherPrecipMedium(O+V*((a+b)*0.5),vec2(0.0),0.0,0.0,0.0,localScattering,localExtinction);
        cmStep(t,air,O,V,a,b,usedHeight,localScattering*(height/0.9),localExtinction,height,src,ext);
    }
    return t;
}
CameraTransport cmFinite(vec3 O,vec3 V,float begin,float end,float usedHeight)
{
    CameraTransport air;
    return cmFiniteWithAir(O,V,begin,end,usedHeight,air);
}
vec3 cmBoundarySky(vec3 O,vec3 V)
{
    const vec3 planet=cmPlanet(O);
    float distance=pkRaySphere(planet,V,pkTop());
    const float ground=pkRaySphere(planet,V,pkBottom());
    if(ground>=0.0) distance=min(distance,ground);
    distance=max(distance*1000.0,0.0);
    CameraTransport sky=cmIdentity();
    for(int i=0;i<32;++i)
    {
        const float a=distance*pow(float(i)/32.0,2.0);
        const float b=distance*pow(float(i+1)/32.0,2.0);
        // Planet curvature controls high-altitude coefficients along long sky rays.
        const vec3 q=planet+V*((a+b)*0.0005);
        vec3 ext,src,height;
        cmLightPlanet(q,V,vec3(1.0),1.0,ext,src,height);
        // Boundary sky contains physical air only. The finite artistic/local prefix
        // is replaced below, exactly as the main sky's AP endpoint contract.
        float begin=a,end=b;
        if(V.z!=0.0)
        {
            const float water=(ssr.haze_mode.z-O.z)/V.z;
            if(V.z>0.0) begin=max(begin,water); else end=min(end,water);
        }
        else if(O.z<ssr.haze_mode.z) end=begin;
        cmSlab(sky,ext,src,max(end-begin,0.0));
    }
    return sky.L;
}
vec3 cmCaptureSky(vec3 O,vec3 V)
{
    CameraTransport air;
    const float end=ssr.haze_mode.w;
    const CameraTransport nearField=cmFiniteWithAir(O,V,0.0,end,0.0,air);
    if(nearField.heightTau==0.0 && all(equal(nearField.L,air.L)) && all(equal(nearField.T,air.T)))
        return cmBoundarySky(O,V);
    if(any(lessThan(air.T,vec3(1e-3))))
        return nearField.L+nearField.T*cmBoundarySky(O+V*end,V);
    const vec3 raw=cmBoundarySky(O,V);
    return raw+(nearField.L-air.L)+(nearField.T-air.T)*((raw-air.L)/air.T);
}
// Cached background approximation for a secondary raster view. It retains
// the shared cached boundary sky; only its finite prefix uses this origin.
vec3 cmCachedSkyPrefix(vec3 raw,vec3 O,vec3 V)
{
    CameraTransport air;
    const CameraTransport combined=cmFiniteWithAir(O,V,0.0,ssr.haze_mode.w,0.0,air);
    if(combined.heightTau==0.0 && all(equal(combined.L,air.L)) && all(equal(combined.T,air.T))) return raw;
    // Avoid an ill-conditioned inverse without regenerating a boundary per pixel.
    if(any(lessThan(air.T,vec3(1e-3)))) return combined.L+combined.T*raw;
    return raw+(combined.L-air.L)+(combined.T-air.T)*((raw-air.L)/air.T);
}
float cmLog1p(float x)
{
    return abs(x)<1e-3 ? x*(1.0-x*0.5+x*x/3.0-x*x*x*0.25) : log(1.0+x);
}
float cmExpm1(float x)
{
    return abs(x)<1e-3 ? x*(1.0+x*0.5+x*x/6.0+x*x*x/24.0) : exp(x)-1.0;
}
// One reserved boundary at D/4 has zero artistic optical depth in EVERY
// column. Both intervals are logarithmic VIEW DEPTH. The start plane curves
// per column so its Euclidean distance is exactly the resolved artistic start.
// Trilinear neighbours at/before that normalized boundary therefore contain
// exactly zero height extinction/source, including oblique frustum corners.
float cmDepthMapping(float u,float factor,float start,float range,float n)
{
    if(u<=0.0) return 0.0;
    if(u>=1.0) return range;
    const float boundary=clamp(start*factor,0.0,range);
    if(u==0.25) return boundary;
    if(u<=0.25) return n*cmExpm1((u*4.0)*cmLog1p(boundary/n));
    return boundary+n*cmExpm1(((u-0.25)/0.75)*cmLog1p((range-boundary)/n));
}
float cmSliceMapping(float depth,float factor,float start,float range,float n)
{
    if(depth<=0.0) return 0.0;
    if(depth>=range) return 1.0;
    const float boundary=clamp(start*factor,0.0,range);
    if(depth<=boundary && boundary>0.0)
        return min(0.25,0.25*cmLog1p(depth/n)/cmLog1p(boundary/n));
    return 0.25+0.75*cmLog1p(max(depth-boundary,0.0)/n)/max(cmLog1p((range-boundary)/n),1e-30);
}
float cmDepth(float u,CameraMediumView view,float factor)
{
    return cmDepthMapping(u,factor,view.mapping.x,view.grid.w,view.slice.x);
}
float cmSlice(float depth,CameraMediumView view,float factor)
{
    return cmSliceMapping(depth,factor,view.mapping.x,view.grid.w,view.slice.x);
}
void cmRay(vec2 uv, uint eye, out vec3 O, out vec3 V)
{
    const CameraMediumView v=ssr.medium[eye];
    const vec2 ndc=uv*2.0-1.0;
    const vec4 p=v.inv_vp*vec4(ndc,0.5,1.0);
    if(abs(p.w)<1e-7) { O=v.eye.xyz;V=v.forward.xyz;return; }
    const vec3 P=v.eye.xyz+p.xyz/p.w;
    V=v.forward.w>0.5 ? v.forward.xyz : normalize(P-v.eye.xyz);
    O=v.forward.w>0.5 ? P-V*dot(P-v.eye.xyz,V) : v.eye.xyz;
}
CameraTransport cmAt(vec3 P, uint eye)
{
    const CameraMediumView v=ssr.medium[eye];
    const vec4 clip=v.vp*vec4(P-v.eye.xyz,1.0);
    if(clip.w<=0.0 || any(isnan(P)) || any(isinf(P))) return cmIdentity();
    const vec2 uv=clamp(clip.xy/max(abs(clip.w),1e-7)*0.5+0.5,0.0,1.0);
    vec3 O,V;cmRay(uv,eye,O,V);
    const float distance=max(dot(P-O,V),0.0);
    const float factor=v.forward.w>0.5 ? 1.0 : max(dot(V,v.forward.xyz),1e-5);
    const float depth=distance*factor;
    if(depth<=0.0) return cmIdentity();
    float c=cmSlice(min(depth,v.grid.w),v,factor)*v.grid.z-1.0;
    if(distance<=v.mapping.x) c=min(c,v.grid.z*0.25-1.0);
    const vec3 coord=vec3(uv,(max(c,0.0)+0.5)/v.grid.z);
    const vec4 transportT=textureLod(uMediumT[eye],coord,0.0);
    CameraTransport t=CameraTransport(textureLod(uMediumL[eye],coord,0.0).rgb,transportT.rgb,transportT.a);
    if(c<0.0)
    {
        const float first=cmDepth(1.0/v.grid.z,v,factor);
        const float f=clamp(depth/max(first,1e-7),0.0,1.0);
        t.L*=f;t.T=mix(vec3(1.0),t.T,f);t.heightTau*=f;
    }
    if(depth>v.grid.w)
    {
        const float begin=v.grid.w/factor;
        const float used=t.heightTau;
        const CameraTransport tail=cmFinite(O,V,begin,distance,used);
        t.L+=t.T*tail.L;t.T*=tail.T;t.heightTau=tail.heightTau;
    }
    return t;
}
vec3 cmApply(vec3 color,CameraTransport t) { return color*t.T+t.L; }
vec3 cmRemove(vec3 color,CameraTransport t,vec3 fallback)
{
    return any(lessThan(t.T,vec3(1e-3))) ? fallback : max(color-t.L,vec3(0.0))/t.T;
}
// Raw sky already contains physical air. Replace only its finite air prefix.
vec3 cmSkyPrefix(vec3 raw,vec3 O,vec3 V,uint eye)
{
    const CameraMediumView view=ssr.medium[eye];
    const float forward=dot(V,view.forward.xyz);
    const float factor=view.forward.w>0.5 ? 1.0 : forward;
    const float end=factor>1e-5 ? view.grid.w/factor : view.grid.w;
    const vec3 P=O+V*end;
    const vec4 clip=view.vp*vec4(P-view.eye.xyz,1.0);
    const vec2 uv=clip.xy/max(abs(clip.w),1e-7)*0.5+0.5;
    CameraTransport air,combined;
    if(factor<=1e-5 || clip.w<=0.0 || any(lessThan(uv,vec2(0.0))) || any(greaterThan(uv,vec2(1.0))))
        combined=cmFiniteWithAir(O,V,0.0,end,0.0,air);
    else
    {
        air=CameraTransport(textureLod(uMediumAirL[eye],uv,0.0).rgb,textureLod(uMediumAirT[eye],uv,0.0).rgb,0.0);
        combined=cmAt(P,eye);
    }
    if(combined.heightTau==0.0 && (view.misc.w<0.5 ||
       (all(equal(combined.L,air.L)) && all(equal(combined.T,air.T))))) return raw;
    if(any(lessThan(air.T,vec3(1e-3))))
        return combined.L+combined.T*raw; // cached-background conditioning approximation
    // Delta form is algebraically the same prefix replacement and returns raw
    // bit-for-bit when the combined and air-only endpoints coincide.
    return raw+(combined.L-air.L)+(combined.T-air.T)*((raw-air.L)/air.T);
}
#endif
