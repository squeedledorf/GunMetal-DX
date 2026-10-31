/** @file llhaze.h
 * Deterministic environment haze resolution and finite height-medium integration.
 */
#ifndef LL_LLHAZE_H
#define LL_LLHAZE_H

#include <algorithm>
#include <array>
#include <cmath>

namespace LLHaze
{
enum class Source { Derived, Sky, Blend, User };
struct Inputs
{
    double hazeDensity = .7, densityMultiplier = .0001, distanceMultiplier = .8;
    double maxY = 1605., illuminationRatio = 1.;
};
struct Optional { bool present = false; double value = 0.; };
struct Saved { Optional startDistance, maxOpacity; };
struct Field { double value = 0.; Source source = Source::Derived; };
struct Pair { Field startDistance, maxOpacity; };
struct Derived
{
    Inputs sanitized;
    double sigma0 = 0., scaleHeight = 10., brightness = 0.;
    Pair automatic;
};
struct Lighting
{
    std::array<double,3> sun{0.,0.,1.},moon{0.,0.,-1.};
    std::array<double,3> light{.7342,.7815,.8999},ambient{.25,.25,.25},blue{.2447,.4487,.7599};
    double moonBrightness=.5;
};
struct Endpoint
{
    Inputs inputs;
    Saved saved;
    bool frozen = false;
    Pair snapshot;
    bool hasLighting=false;
    Lighting lighting;
};
struct Transition { Endpoint from, to; double weight = 0.; };

inline double bounded(double value, double fallback, double lower, double upper)
{
    return std::clamp(std::isfinite(value) ? value : fallback, lower, upper);
}
inline double smooth(double lower, double upper, double value)
{
    const double t = bounded((value - lower) / (upper - lower), 0., 0., 1.);
    return t * t * (3. - 2. * t);
}
inline Derived derive(const Inputs& in)
{
    Derived out;
    auto& s = out.sanitized;
    s.hazeDensity = bounded(in.hazeDensity, .7, 0., 5.);
    s.densityMultiplier = bounded(in.densityMultiplier, .0001, 0., .1);
    s.distanceMultiplier = bounded(in.distanceMultiplier, .8, 0., 1000.);
    s.maxY = bounded(in.maxY, 1605., 0., 10000.);
    s.illuminationRatio = bounded(in.illuminationRatio, 1., 0., 1.e6);
    out.sigma0 = std::clamp(s.hazeDensity * s.densityMultiplier * s.distanceMultiplier, 0., .1);
    out.scaleHeight = std::clamp(.02 * s.maxY, 10., 200.);
    const double tau = out.sigma0 * 128. * std::exp(-2. / out.scaleHeight);
    const double q = tau / (tau + .08);
    out.brightness = smooth(.01, .25, s.illuminationRatio);
    out.automatic.startDistance.value = std::clamp((1. - q) * (8. + 24. * (1. - out.brightness)), 0., 32.);
    out.automatic.maxOpacity.value = std::clamp(.55 + .30 * q - .15 * (1. - out.brightness), .40, .85);
    return out;
}
inline Field savedOrDerived(Field automatic, Optional saved, double upper)
{
    if (saved.present && std::isfinite(saved.value) && saved.value >= 0.)
        return {std::clamp(saved.value, 0., upper), Source::Sky};
    return automatic;
}
inline Pair resolveEndpoint(const Inputs& inputs, const Saved& saved)
{
    Pair out = derive(inputs).automatic;
    out.startDistance = savedOrDerived(out.startDistance, saved.startDistance, 512.);
    out.maxOpacity = savedOrDerived(out.maxOpacity, saved.maxOpacity, .95);
    return out;
}
inline Pair resolveEndpoint(const Endpoint& endpoint)
{
    return endpoint.frozen ? endpoint.snapshot : resolveEndpoint(endpoint.inputs, endpoint.saved);
}
inline Pair blend(const Pair& from, const Pair& to, double weight)
{
    const double t = bounded(weight, 0., 0., 1.);
    if (t == 0.) return from;
    if (t == 1.) return to;
    const auto field = [t](Field a, Field b)
    {
        return Field{a.value + (b.value - a.value) * t,
                     a.source == b.source && a.value == b.value ? a.source : Source::Blend};
    };
    return {field(from.startDistance, to.startDistance), field(from.maxOpacity, to.maxOpacity)};
}
inline Pair resolveTransition(const Transition& transition)
{
    return blend(resolveEndpoint(transition.from), resolveEndpoint(transition.to), transition.weight);
}
inline Endpoint freeze(const Transition& transition)
{
    Endpoint out;
    out.frozen = true;
    out.snapshot = resolveTransition(transition);
    return out;
}
inline Pair overrideLocal(Pair out, double startPreference = -1., double opacityPreference = -1.)
{
    if (std::isfinite(startPreference) && startPreference >= 0.)
        out.startDistance = {std::clamp(startPreference, 0., 512.), Source::User};
    if (std::isfinite(opacityPreference) && opacityPreference >= 0.)
        out.maxOpacity = {std::clamp(opacityPreference, 0., .95), Source::User};
    return out;
}
inline const char* sourceName(Source source)
{
    switch (source)
    {
        case Source::Sky: return "sky";
        case Source::Blend: return "blend";
        case Source::User: return "user";
        default: return "derived";
    }
}

// The shader's shared fixed-grid mapping, including collapsed intervals. The
// normalized quarter boundary always represents the same Euclidean start in
// neighbouring columns; depth remains measured along the view's forward axis.
inline float sliceDepth(float u,float factor,float start,float range,float n)
{
    if(u<=0.f) return 0.f;
    if(u>=1.f) return range;
    const float boundary=std::clamp(start*factor,0.f,range);
    if(u==.25f) return boundary;
    if(u<=.25f) return n*std::expm1((u*4.f)*std::log1p(boundary/n));
    return boundary+n*std::expm1(((u-.25f)/.75f)*std::log1p((range-boundary)/n));
}
inline float sliceCoordinate(float depth,float factor,float start,float range,float n)
{
    if(depth<=0.f) return 0.f;
    if(depth>=range) return 1.f;
    const float boundary=std::clamp(start*factor,0.f,range);
    if(depth<=boundary && boundary>0.f)
        return std::min(.25f,.25f*std::log1p(depth/n)/std::log1p(boundary/n));
    return .25f+.75f*std::log1p(std::max(depth-boundary,0.f)/n)/std::max(std::log1p((range-boundary)/n),1.e-30f);
}
inline float prefixIndex(float distance,float factor,float start,float range,float n,float slices)
{
    float index=sliceCoordinate(distance*factor,factor,start,range,n)*slices-1.f;
    if(distance<=start) index=std::min(index,slices*.25f-1.f);
    return index;
}

// Exact integral of sigma0*exp(-max(z-base,0)/H), clipped to the air side of
// water and to the artistic start distance. originZ and base are in one world datum;
// directionZ belongs to a unit ray. Evaluate downhill from the lower end, avoiding
// exp(+large)*exp(-large). Horizontal rays use the continuous expm1 limit.
inline double heightOpticalDepth(double sigma0, double scaleHeight, double base,
                                 double water, double originZ, double directionZ,
                                 double begin, double end, double start = 0.)
{
    begin = std::max({begin, start, 0.});
    if (!(end > begin) || !(sigma0 > 0.) || !(scaleHeight > 0.)) return 0.;
    if (directionZ == 0.)
        return originZ < water ? 0. : sigma0 * (end - begin) * std::exp(-std::max(originZ - base, 0.) / scaleHeight);
    const double crossing = (water - originZ) / directionZ;
    if (directionZ > 0.) begin = std::max(begin, crossing);
    else end = std::min(end, crossing);
    if (!(end > begin)) return 0.;
    const double za = originZ + directionZ * begin, zb = originZ + directionZ * end;
    const double low = std::min(za, zb);
    const double distance=end-begin;
    const double dz=std::abs(directionZ)*distance;
    if(dz==0.) return sigma0*distance*std::exp(-std::max(low-base,0.)/scaleHeight);
    const double below=std::clamp(base-low,0.,dz);
    const double above=dz-below;
    const double x=above/scaleHeight;
    const double mean=x==0. ? 1. : -std::expm1(-x)/x;
    return sigma0*distance*(below/dz+std::exp(-std::max(low-base,0.)/scaleHeight)*(above/dz)*mean);
}
inline double opacityOpticalDepth(double opacity)
{
    return -std::log1p(-bounded(opacity, 0., 0., .95));
}
inline double consumeHeight(double raw, double cap, double& used)
{
    const double delta = std::clamp(raw, 0., std::max(cap - used, 0.));
    used += delta;
    return delta;
}
// A homogeneous source j integrated through extinction sigma over length ds.
inline double slabSource(double source, double extinction, double distance)
{
    return extinction > 0. ? source * -std::expm1(-extinction * distance) / extinction : source * distance;
}
} // namespace LLHaze
#endif
