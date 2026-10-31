/** @file llhazesky.h
 * Semantic sky adapter for the dependency-free haze resolver.
 */
#ifndef LL_LLHAZESKY_H
#define LL_LLHAZESKY_H
#include "llhaze.h"
#include <atomic>
#include "llsettingssky.h"

namespace LLHaze
{
struct IlluminationPolicy
{
    std::atomic<bool> physicalNight{true};
    std::atomic<double> moonGain{1.}; // explicit source compensation, never screen adaptation
};
// The viewer publishes policy, never exposure or local preference values. The pure
// resolver receives its evaluated numerical inputs; inventory tools use the default.
inline IlluminationPolicy& illuminationPolicy()
{
    static IlluminationPolicy policy;
    return policy;
}
inline double linearColor(double c)
{
    c = bounded(c, 0., 0., 64.);
    return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
}
inline double moonFraction(double sunMoonDot, double moonBrightness, double scale = .05)
{
    constexpr double pi = 3.141592653589793;
    const double phase = 180. - std::acos(bounded(sunMoonDot,-1.,-1.,1.)) * (180. / pi);
    return scale * bounded(2. * moonBrightness, 1., 0., 2.) *
           std::pow(10., -.4 * (.026 * phase + 4.e-9 * phase * phase * phase * phase));
}
// Same linear source convention as the physical tables: EEP light x8, diffuse x.3.
// A fixed ground-level estimate of visible illumination, independent of camera,
// exposure, draw range and the artistic height density. Physical air uses only the
// bounded blue-density Rayleigh ratio plus the default Earth Mie column.
inline double illumination(const Lighting& lighting, bool physicalNight, double moonGain=1.)
{
    constexpr double pi = 3.141592653589793;
    constexpr double ray[3] = {.005802, .013558, .0331};
    constexpr double blueDefault[3] = {.2447, .4487, .7599};
    constexpr double luma[3] = {.2126, .7152, .0722};
    const auto& sun=lighting.sun;const auto& moon=lighting.moon;
    const auto& light=lighting.light;const auto& ambient=lighting.ambient;const auto& blue=lighting.blue;
    const double elevation = std::asin(bounded(sun[2], 1., -1., 1.)) * 180. / pi;
    const double moonScale = physicalNight ? moonFraction(sun[0]*moon[0]+sun[1]*moon[1]+sun[2]*moon[2],lighting.moonBrightness) *
                                              (1. - smooth(-10., 4., elevation))*bounded(moonGain,1.,0.,64.) : (1.-smooth(-10.,4.,elevation));
    const double skyScale = physicalNight ? std::clamp(std::max(smooth(-5., 0., elevation), moonScale), 0., 1.) : 1.;
    double energy = 0.;
    for (int c = 0; c < 3; ++c)
    {
        const double optical = ray[c] * bounded(blue[c] / blueDefault[c], 1., 0., 6.) * 8. + .004440 * 1.2;
        const auto visible = [optical](double z)
        {
            return z > 0. ? std::exp(-optical / std::max(z, .01)) : 0.;
        };
        const double direct = 8. * linearColor(light[c]) * (visible(sun[2]) + moonScale * visible(moon[2]));
        // Twilight contributes pre-exposure diffuse light even while direct sun is hidden.
        const double twilight = physicalNight && elevation < 0. ? .12 * std::exp2(.7 * elevation) * smooth(-12., -9., elevation) : 0.;
        const double diffuse = .3 * linearColor(ambient[c]) * skyScale + 8. * linearColor(light[c]) * twilight;
        energy += luma[c] * (direct + pi * diffuse);
    }
    return std::max(energy, 0.);
}
inline double referenceIllumination()
{
    constexpr double luma[3] = {.2126, .7152, .0722};
    constexpr double ray[3] = {.005802, .013558, .0331};
    constexpr double light[3] = {.7342, .7815, .8999};
    double energy = 0.;
    for (int c = 0; c < 3; ++c)
        energy += luma[c] * (8. * linearColor(light[c]) * std::exp(-ray[c] * 8. - .004440 * 1.2) +
                            3.141592653589793 * .3 * linearColor(.25));
    return energy;
}
}
inline void llHazeRefreshTransition(LLHaze::Transition& transition)
{
    const bool physicalNight=LLHaze::illuminationPolicy().physicalNight.load(std::memory_order_relaxed);
    const double moonGain=LLHaze::illuminationPolicy().moonGain.load(std::memory_order_relaxed);
    for(auto* endpoint : {&transition.from,&transition.to})
        if(!endpoint->frozen && endpoint->hasLighting)
            endpoint->inputs.illuminationRatio=LLHaze::illumination(endpoint->lighting,physicalNight,moonGain)/LLHaze::referenceIllumination();
}
inline LLHaze::Endpoint llHazeEndpoint(const LLSettingsSky& sky)
{
    LLHaze::Endpoint out;
    const auto sun=sky.getSunDirection(),moon=sky.getMoonDirection();
    const auto light=sky.getSunlightColor(),ambient=sky.getAmbientColor(),blue=sky.getBlueDensity();
    for(int c=0;c<3;++c)
    {
        out.lighting.sun[c]=sun.mV[c];out.lighting.moon[c]=moon.mV[c];
        out.lighting.light[c]=light.mV[c];out.lighting.ambient[c]=ambient.mV[c];out.lighting.blue[c]=blue.mV[c];
    }
    out.lighting.moonBrightness=sky.getMoonBrightness();out.hasLighting=true;
    out.inputs = {sky.getHazeDensity(), sky.getDensityMultiplier(), sky.getDistanceMultiplier(), sky.getMaxY(),
                  LLHaze::illumination(out.lighting,LLHaze::illuminationPolicy().physicalNight,LLHaze::illuminationPolicy().moonGain)/LLHaze::referenceIllumination()};
    const LLSD& settings = const_cast<LLSettingsSky&>(sky).getSettings();
    const auto optional = [&settings](const char* key)
    {
        const LLSD& value = settings[key];
        const bool numeric = value.isReal() || value.isInteger();
        return LLHaze::Optional{settings.has(key) && numeric && std::isfinite(value.asReal()), numeric ? value.asReal() : 0.};
    };
    out.saved.startDistance = optional("sl_haze_start_distance");
    out.saved.maxOpacity = optional("sl_haze_max_opacity");
    return out;
}
#endif
